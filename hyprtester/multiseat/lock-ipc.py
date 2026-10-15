"""Targeted sessionlock IPC regression in a private, nested compositor.

Run with /usr/bin/python3 lock-ipc.py --binary /path/to/Hyprland
--render-node /dev/dri/renderD128. Only the selected render node is exposed;
physical displays, input devices, the user's HOME and desktop sockets are hidden.
Requires bwrap, Mutter, a C compiler, wayland-scanner and wayland-client.
"""

import argparse
import json
import os
import resource
from pathlib import Path
import select
import shlex
import signal
import socket
import subprocess
import tempfile
import time


def launch_sandbox(args):
    source = Path(__file__).resolve().parent
    binary = Path(args.binary).resolve(strict=True)
    artifacts = Path(tempfile.mkdtemp(prefix="hyprland-lock-ipc-"))
    (artifacts / "home").mkdir()
    protocol = "/usr/share/wayland-protocols/staging/ext-session-lock/ext-session-lock-v1.xml"
    for mode, suffix in (("client-header", "h"), ("private-code", "c")):
        subprocess.run(["wayland-scanner", mode, protocol, str(artifacts / ("session-lock." + suffix))], check=True)
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client"], text=True))
    subprocess.run(["cc", "-I" + str(artifacts), str(source / "lock.c"), str(artifacts / "session-lock.c"), "-o", str(artifacts / "lock"), *flags], check=True)
    command = ["bwrap", "--unshare-all", "--die-with-parent", "--new-session",
               "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
               "--tmpfs", "/run", "--tmpfs", "/tmp", "--tmpfs", "/var/tmp",
               "--tmpfs", "/home", "--tmpfs", "/root",
               "--ro-bind", str(source), str(source), "--ro-bind", str(binary), str(binary),
               "--bind", str(artifacts), "/tmp/t", "--chdir", "/tmp/t",
               "--clearenv", "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "LANG", "C.UTF-8",
               "--setenv", "HOME", "/tmp/t/home", "--setenv", "USER", "lock-ipc-test",
               "--setenv", "DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/no-system-bus",
               "--setenv", "HYPRLAND_LOCK_TEST_SANDBOX", "1"]
    if args.render_node:
        node = Path(args.render_node).resolve(strict=True)
        if node.parent != Path("/dev/dri") or not node.name.startswith("renderD") or not node.is_char_device():
            raise ValueError("Only a DRM render node may be exposed")
        command += ["--dir", "/dev/dri", "--dev-bind", str(node), str(node)]
    else:
        command += ["--setenv", "LIBGL_ALWAYS_SOFTWARE", "1", "--setenv", "GALLIUM_DRIVER", "llvmpipe"]
    command += ["--", "/usr/bin/python3", str(source / "lock-ipc.py"), "--inside", "--binary", str(binary)]
    print("Isolated evidence:", artifacts, flush=True)
    # subprocess closes inherited descriptors, including any ambient Wayland FD.
    return subprocess.call(command, close_fds=True)


def verify(binary):
    assert os.environ.get("HYPRLAND_LOCK_TEST_SANDBOX") == "1", "Use the sandbox launcher"
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    base = Path("/tmp/t")
    runtime = base / "r"
    runtime.mkdir(mode=0o700)
    env = os.environ | {"XDG_RUNTIME_DIR": str(runtime), "XDG_CONFIG_HOME": str(base / "config"),
                        "XDG_CACHE_HOME": str(base / "cache"), "XDG_STATE_HOME": str(base / "state"),
                        "AQ_DRM_DEVICES": "/dev/null", "LIBSEAT_BACKEND": "noop", "NO_AT_BRIDGE": "1"}
    processes = []
    bus_pid = None
    events = []
    pending = b""

    def wait(callback, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = callback()
            if result:
                return result
            time.sleep(.02)
        raise AssertionError("Readiness timeout; sessionlock events: " + repr(events))

    def start(command, name, **kwargs):
        output = kwargs.pop("stdout", open(base / (name + ".log"), "w"))
        process = subprocess.Popen(command, env=env, start_new_session=True, stdout=output,
                                   stderr=open(base / (name + ".stderr"), "w"), **kwargs)
        processes.append(process)
        return process

    def ctl(command, as_json=False):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(5)
            connection.connect(str(runtime / "hypr" / env["HYPRLAND_INSTANCE_SIGNATURE"] / ".socket.sock"))
            connection.sendall((("j" if as_json else "") + "/" + command).encode())
            chunks = []
            while chunk := connection.recv(65536):
                chunks.append(chunk)
        answer = b"".join(chunks).decode().strip()
        return json.loads(answer) if as_json else answer

    def pump(timeout=.1):
        nonlocal pending
        if select.select([stream], [], [], timeout)[0]:
            chunk = stream.recv(65536)
            assert chunk, "Compositor event stream closed"
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if line.startswith(b"sessionlock>>"):
                    events.append(line.decode().split(">>", 1)[1])

    def expect(expected):
        def ready():
            pump()
            assert events == expected[:len(events)], (events, expected)
            return len(events) == len(expected)
        wait(ready)

    def locker(name):
        process = start([str(base / "lock")], name, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert select.select([process.stdout], [], [], 10)[0] and process.stdout.readline().strip() == "ready"
        assert ctl("locked", True)["locked"]
        return process

    try:
        config = base / "hyprland.lua"
        config.write_text('hl.monitor({output="",mode="1280x800",position="auto",scale=1})\n'
                          'hl.config({animations={enabled=false},xwayland={enabled=false},'
                          'misc={disable_hyprland_logo=true,disable_splash_rendering=true,force_default_wallpaper=0}})\n')
        bus = subprocess.check_output(["dbus-daemon", "--session", "--fork", "--print-address=1", "--print-pid=1"], env=env, text=True).splitlines()
        env["DBUS_SESSION_BUS_ADDRESS"] = bus[0]
        bus_pid = int(bus[1])
        start(["mutter", "--headless", "--wayland", "--no-x11", "--wayland-display=parent", "--virtual-monitor", "1280x800"], "parent")
        wait(lambda: (runtime / "parent").is_socket())
        env["WAYLAND_DISPLAY"] = "parent"
        compositor = start([binary, "-c", str(config)], "Hyprland")
        env["HYPRLAND_INSTANCE_SIGNATURE"] = wait(lambda: next((p.name for p in (runtime / "hypr").glob("*") if (p / ".socket.sock").is_socket()), None))
        assert ctl("output create headless human") == "ok"
        assert ctl('eval hl.monitor({output="human",mode="1280x800",position="0x0",scale=1})') == "ok"
        for monitor in ctl("monitors", True):
            if monitor["name"] != "human":
                assert ctl('eval hl.monitor({output=' + json.dumps(monitor["name"]) + ',disabled=true})') == "ok"
        env["WAYLAND_DISPLAY"] = wait(lambda: next((p.name for p in runtime.glob("wayland-*") if p.is_socket()), None))
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as event_stream:
            stream = event_stream
            stream.connect(str(runtime / "hypr" / env["HYPRLAND_INSTANCE_SIGNATURE"] / ".socket2.sock"))
            assert not ctl("locked", True)["locked"]  # Also synchronize the socket accept.
            for iteration in range(2):
                process = locker("normal-" + str(iteration))
                expect(["locked", "unlocked"] * iteration + ["locked"])
                process.stdin.write("unlock\n")
                process.stdin.flush()
                assert process.wait(timeout=5) == 0
                expect(["locked", "unlocked"] * (iteration + 1))
                assert not ctl("locked", True)["locked"]
            process = locker("orphaned")
            expect(["locked", "unlocked", "locked", "unlocked", "locked"])
            process.kill()
            process.wait(timeout=5)
            assert ctl("locked", True)["locked"]
            pump(.1)
            assert events == ["locked", "unlocked", "locked", "unlocked", "locked"]
            # Wait until the compositor has processed the dead lock owner.
            def recover():
                answer = ctl("eval hl.clear_crashed_lockscreen()")
                return answer == "ok"
            wait(recover)
            expect(["locked", "unlocked"] * 3)
            assert not ctl("locked", True)["locked"]
            assert ctl("eval hl.clear_crashed_lockscreen()") != "ok"
            pump(.3)
            assert events == ["locked", "unlocked"] * 3, events
            assert not ctl("locked", True)["locked"]
            assert compositor.poll() is None
        print(json.dumps({"result": "passed", "events": events,
                          "checks": ["normal unlock twice", "lock owner crash and forceUnlock/clearSessionLock",
                                     "redundant recovery refused without extra events", "compositor locked state matches notifications"]}), flush=True)
    finally:
        (base / "events.json").write_text(json.dumps(events, indent=2) + "\n")
        for process in reversed(processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
        for process in reversed(processes):
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
        if bus_pid:
            try:
                os.kill(bus_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--render-node")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.inside:
        verify(args.binary)
    else:
        raise SystemExit(launch_sandbox(args))
