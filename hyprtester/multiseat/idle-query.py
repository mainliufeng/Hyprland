"""Query real mapped/unmapped synthetic windows while idle inhibitors exist.

Only run through sandbox.sh: never query an ambient compositor.
"""
import json
import os
import pathlib
import select
import subprocess
import time
from runtime import Runtime, wait

SOURCE = pathlib.Path(__file__).resolve().parent
runtime = Runtime("idle-query")
checks = []


def command(process, value):
    process.stdin.write(value + "\n")
    process.stdin.flush()
    assert select.select([process.stdout], [], [], 5)[0], value
    assert process.stdout.readline().strip() == "done", value


def query(label, expected):
    tool = os.environ["HYPRLAND_TEST_HYPRCTL"]
    result = subprocess.run([tool, "-j", "-a", "clients"], env=runtime.env, capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, (label, result.returncode, result.stdout, result.stderr, runtime.compositor.poll())
    assert runtime.compositor.poll() is None, (label, "compositor exited", runtime.compositor.returncode)
    try:
        windows = {w["title"]: w for w in json.loads(result.stdout)}
    except json.JSONDecodeError as error:
        raise AssertionError((label, "all-clients query did not return JSON", result.stdout)) from error
    for title, (mapped, inhibiting) in expected.items():
        assert title in windows, (label, title, windows)
        assert (windows[title]["mapped"], windows[title]["inhibitingIdle"]) == (mapped, inhibiting), (label, windows[title])
    text = subprocess.run([tool, "-a", "clients"], env=runtime.env, capture_output=True, text=True, timeout=5)
    assert text.returncode == 0 and all(title in text.stdout for title in expected), (label, text.returncode)
    assert runtime.compositor.poll() is None, label
    checks.append(label)
    (runtime.base / "idle-query.json").write_text(json.dumps(checks, indent=2))
    print("PASS", label, flush=True)


try:
    with runtime.config.open("a") as config:
        config.write("hl.config({xwayland={enabled=true}})\n")
    runtime.launch()
    protocols = pathlib.Path(subprocess.check_output(["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True).strip())
    sources = {"xdg": protocols / "stable/xdg-shell/xdg-shell.xml", "idle": protocols / "unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"}
    for name, path in sources.items():
        for mode, ext in (("client-header", "h"), ("private-code", "c")):
            subprocess.run(["wayland-scanner", mode, str(path), str(runtime.base / (name + "." + ext))], check=True)
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client"], text=True).split()
    subprocess.run(["cc", "-I" + str(runtime.base), str(SOURCE / "idle-client.c"), *(str(runtime.base / (name + ".c")) for name in sources), "-o", str(runtime.base / "client"), *flags], check=True)
    client = runtime.start([str(runtime.base / "client")], "client", stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert select.select([client.stdout], [], [], 5)[0] and client.stdout.readline().strip() == "ready"
    command(client, "create idle-visible")
    command(client, "map 0")
    wait(lambda: runtime.ctl("clients", True))
    command(client, "inhibit 0")
    query("mapped inhibitor remains effective", {"idle-visible": (True, True)})
    # Xwayland retains an unmapped Window after its Wayland surface is gone.
    # This is the missing-surface state from the real clients -a crash report.
    xflags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "x11"], text=True).split()
    subprocess.run(["cc", str(SOURCE / "idle-x11-client.c"), "-o", str(runtime.base / "x11-client"), *xflags], check=True)
    display_file = runtime.base / "x-display"
    runtime.ok('dispatch hl.dsp.exec_raw(' + json.dumps("sh -c 'printf %s \"$DISPLAY\" > " + str(display_file) + "'") + ')')
    wait(lambda: display_file.exists() and display_file.read_text().startswith(":"))
    xclient = runtime.start([str(runtime.base / "x11-client")], "x11-client",
                            env=runtime.env | {"DISPLAY": display_file.read_text()},
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert select.select([xclient.stdout], [], [], 10)[0] and xclient.stdout.readline().strip() == "ready"
    command(xclient, "map")
    wait(lambda: any(w["title"] == "idle-x11-retained" for w in runtime.ctl("clients", True)))
    query("mapped Xwayland window with a Wayland idle inhibitor", {"idle-visible": (True, True), "idle-x11-retained": (True, False)})
    command(xclient, "unmap")
    wait(lambda: not any(w["title"] == "idle-x11-retained" for w in runtime.ctl("clients", True)))
    query("Xwayland retained window without a wl_surface", {"idle-visible": (True, True), "idle-x11-retained": (False, False)})
    command(client, "create idle-never-mapped")
    query("never mapped window with another active inhibitor", {"idle-visible": (True, True), "idle-never-mapped": (False, False)})
    command(client, "map 1")
    wait(lambda: len(runtime.ctl("clients", True)) == 2)
    command(client, "unmap 1")
    query("unmapped retained window with another active inhibitor", {"idle-visible": (True, True), "idle-never-mapped": (False, False)})
    command(client, "unmap 0")
    query("unmapped inhibitor is ineffective", {"idle-visible": (False, False), "idle-never-mapped": (False, False)})
    command(client, "map 0")
    wait(lambda: len(runtime.ctl("clients", True)) == 1)
    query("remapped inhibitor becomes effective again", {"idle-visible": (True, True)})
    command(client, "release 0")
    query("destroyed inhibitor is ineffective", {"idle-visible": (True, False)})
    command(client, "inhibit 0")
    for i in range(2, 12):
        command(client, "create idle-cycle-" + str(i))
        query("lifecycle query " + str(i), {"idle-visible": (True, True), "idle-cycle-" + str(i): (False, False)})
        command(client, "destroy " + str(i))
    command(client, "destroy 0")
    query("inhibitor surface destruction leaves compositor responsive", {"idle-never-mapped": (False, False)})
finally:
    runtime.cleanup()
