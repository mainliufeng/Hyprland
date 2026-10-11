"""Shared real compositor lifecycle for isolated integration tests."""
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time


def wait(callback, timeout=25, ready=bool):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = callback()
            if ready(value):
                return value
        except (OSError, ValueError, StopIteration, subprocess.SubprocessError):
            pass
        time.sleep(0.05)
    raise RuntimeError("readiness timeout")


class Runtime:
    def __init__(self, name):
        assert os.environ.get("HYPRLAND_TEST_SANDBOX") == "1", "Use sandbox.sh"
        assert not pathlib.Path("/dev/input").exists()
        artifacts = pathlib.Path(os.environ["MULTISEAT_TEST_DIR"]).resolve()
        assert artifacts.is_relative_to(pathlib.Path("/tmp/t")), "Runtime artifacts must stay inside the private sandbox"
        self.base = pathlib.Path(tempfile.mkdtemp(prefix=name + "-", dir=artifacts))
        # Socket paths include a full commit hash and compositor instance ID.
        # Keep their runtime parent short independently of descriptive logs.
        self.rt = pathlib.Path(tempfile.mkdtemp(prefix="r-", dir=artifacts))
        assert all(directory.resolve().is_relative_to(artifacts) for directory in (self.base, self.rt))
        self.rt.chmod(0o700)
        for directory in ("config", "data", "cache", "state"):
            (self.base / directory).mkdir(mode=0o700)
        self.env = os.environ.copy()
        for key in ("HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            self.env.pop(key, None)
        self.env.update(XDG_RUNTIME_DIR=str(self.rt), XDG_CONFIG_HOME=str(self.base / "config"), XDG_DATA_HOME=str(self.base / "data"),
                        XDG_CACHE_HOME=str(self.base / "cache"), XDG_STATE_HOME=str(self.base / "state"),
                        GCOV_PREFIX=str(self.base / "coverage"), GDK_BACKEND="wayland", GTK_IM_MODULE="wayland",
                        GIO_USE_VFS="local", GSETTINGS_BACKEND="memory", NO_AT_BRIDGE="1")
        self.processes = []
        self.config = self.base / "hyprland.lua"
        # Startup news and donation windows are unrelated to seat isolation.
        # Disable them even when a compositor restarts within the same runtime.
        self.config.write_text('hl.monitor({output="",mode="1280x800",position="auto",scale=1})\n'
                               'hl.config({animations={enabled=false},xwayland={enabled=false},misc={disable_hyprland_logo=true,disable_splash_rendering=true,force_default_wallpaper=0},ecosystem={no_update_news=true,no_donation_nag=true},debug={enable_stdout_logs=true}})\n')
        bus = self.start(["dbus-daemon", "--session", "--nofork", "--print-address=1"], "bus", stdout=subprocess.PIPE, text=True)
        self.env["DBUS_SESSION_BUS_ADDRESS"] = bus.stdout.readline().strip()
        assert self.env["DBUS_SESSION_BUS_ADDRESS"].startswith("unix:"), "private bus did not start"
        self.start(["mutter", "--headless", "--wayland", "--no-x11", "--wayland-display=parent", "--virtual-monitor", "1280x800"], "mutter")
        wait(lambda: (self.rt / "parent").is_socket())
        self.env["WAYLAND_DISPLAY"] = "parent"
        self.compositor = None

    def start(self, command, name, **kwargs):
        process = subprocess.Popen(command, env=kwargs.pop("env", self.env),
                                   stdout=kwargs.pop("stdout", open(self.base / (name + ".log"), "w")),
                                   stderr=open(self.base / (name + ".stderr"), "w"), start_new_session=True, **kwargs)
        self.processes.append(process)
        return process

    def ctl(self, command, as_json=False):
        path = self.rt / "hypr" / self.env["HYPRLAND_INSTANCE_SIGNATURE"] / ".socket.sock"
        assert path.is_relative_to(self.rt)
        assert len(os.fsencode(path)) < 108, "IPC socket path exceeds sockaddr_un capacity"
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(str(path))
            connection.sendall((("j" if as_json else "") + "/" + command).encode())
            chunks = []
            while data := connection.recv(65536):
                chunks.append(data)
        answer = b"".join(chunks).decode().strip()
        if not as_json:
            return answer
        try:
            return json.loads(answer)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"Invalid JSON response for IPC {command!r}: {answer!r}") from error

    def ok(self, command):
        answer = self.ctl(command)
        assert answer == "ok", (command, answer)

    def launch(self):
        old_signatures = {p.name for p in (self.rt / "hypr").glob("*")}
        self.env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        self.env["WAYLAND_DISPLAY"] = "parent"
        self.compositor = self.start([self.env["MULTISEAT_HYPRLAND"], "-c", str(self.config)], "Hyprland-" + str(len(self.processes)))
        self.env["HYPRLAND_INSTANCE_SIGNATURE"] = wait(lambda: next((p.name for p in (self.rt / "hypr").glob("*") if p.name not in old_signatures and (p / ".socket.sock").is_socket()), None))
        # An isolated headless compositor starts with no outputs. Successful
        # list decoding establishes readiness even when that list is empty.
        monitors = wait(lambda: self.ctl("monitors", True), ready=lambda value: isinstance(value, list))
        self.ok("output create headless physical")
        self.ok('eval hl.monitor({output="physical",mode="1280x800",position="0x0",scale=1})')
        for monitor in monitors:
            self.ok('eval hl.monitor({output=' + json.dumps(monitor["name"]) + ',disabled=true})')
        wait(lambda: len(self.ctl("monitors", True)) == 1)
        def compositor_display():
            # The native instance lock records the display this exact process
            # bound. A restarted compositor can replace a stale socket using
            # its previous name, while unrelated old seat sockets remain.
            lock = self.rt / "hypr" / self.env["HYPRLAND_INSTANCE_SIGNATURE"] / "hyprland.lock"
            pid, display = lock.read_text().splitlines()
            if int(pid) != self.compositor.pid or not display or pathlib.Path(display).name != display:
                return None
            return display if (self.rt / display).is_socket() else None

        self.env["WAYLAND_DISPLAY"] = wait(compositor_display)
        self.ok('dispatch hl.dsp.focus({monitor="physical"})')
        self.ok('dispatch hl.dsp.focus({workspace="1"})')
        assert not self.ctl("configerrors"), self.ctl("configerrors")
        return self

    def stop(self, process, sig=signal.SIGTERM):
        if process.poll() is None:
            os.killpg(process.pid, sig)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)

    def cleanup(self):
        for process in reversed(self.processes):
            self.stop(process)
        print("Artifacts:", self.base, "Runtime:", self.rt, flush=True)
