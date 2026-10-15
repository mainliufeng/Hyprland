"""First-publication routing using real standard Wayland requests.

All processes, buffers, bus names and outputs belong to Runtime's private
sandbox. No host process command lines/environments or desktop content.
"""
import json
import os
import pathlib
import select
import socket
import subprocess
import threading
import time
from runtime import Runtime, wait

SOURCE = pathlib.Path(__file__).resolve().parent
runtime = Runtime("routing")
checks, publications = [], []
stop_events = threading.Event()


def record(description, evidence):
    checks.append({"description": description, "evidence": evidence})
    print("PASS", description, flush=True)
    (runtime.base / "routing.json").write_text(json.dumps({"checks": checks, "publications": publications}, indent=2))


def client(title):
    return next((w for w in runtime.ctl("clients", True) if w.get("mapped") and w["title"] == title), None)


def command(process, value):
    process.stdin.write(value + "\n"); process.stdin.flush()
    assert select.select([process.stdout], [], [], 5)[0], value
    answer = process.stdout.readline().strip()
    assert answer.startswith("done"), (value, answer, process.poll())
    return answer[5:] if answer.startswith("done ") else ""


def new_client(label, display=None):
    environment = runtime.env.copy()
    if display: environment["WAYLAND_DISPLAY"] = display
    process = runtime.start([str(runtime.base / "client")], label, env=environment,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert select.select([process.stdout], [], [], 5)[0] and process.stdout.readline().strip() == "ready", label
    return process


def context(name):
    state = runtime.ctl("seat state " + name, True)
    return runtime.ctl("seat route-context " + " ".join((name, state["seatId"], str(state["generation"]), state["workspace"])), True)


def publish(process, title, index=0):
    command(process, "create " + title); command(process, "map " + str(index))
    return wait(lambda: client(title))


def event_reader():
    path = runtime.rt / "hypr" / runtime.env["HYPRLAND_INSTANCE_SIGNATURE"] / ".socket2.sock"
    with socket.socket(socket.AF_UNIX) as connection:
        connection.connect(str(path)); connection.settimeout(.2)
        pending = b""
        while not stop_events.is_set():
            try: data = connection.recv(65536)
            except socket.timeout: continue
            if not data: break
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                value = line.decode()
                if value.startswith(("openwindow>>", "movewindow>>", "activewindowv2>>")):
                    publications.append(value)


try:
    runtime.launch()
    runtime.ok("eval hl.config({misc={focus_on_activate=true}})")
    protocol_dir = pathlib.Path(subprocess.check_output(["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True).strip())
    protocols = {"xdg": protocol_dir / "stable/xdg-shell/xdg-shell.xml",
                 "foreign": protocol_dir / "unstable/xdg-foreign/xdg-foreign-unstable-v2.xml",
                 "activation": protocol_dir / "staging/xdg-activation/xdg-activation-v1.xml"}
    for name, xml in protocols.items():
        for mode, extension in (("client-header", "h"), ("private-code", "c")):
            subprocess.run(["wayland-scanner", mode, str(xml), str(runtime.base / (name + "." + extension))], check=True)
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client"], text=True).split()
    subprocess.run(["cc", "-I" + str(runtime.base), str(SOURCE / "routing-client.c"),
                    *(str(runtime.base / (name + ".c")) for name in protocols), "-o", str(runtime.base / "client"), *flags], check=True)
    event_thread = threading.Thread(target=event_reader, daemon=True); event_thread.start(); time.sleep(.1)
    for name in ("A", "B"):
        runtime.ok("seat create " + name + " physical")
        runtime.ok("seat workspace " + name + " name:route-" + name)
    human = new_client("human")
    publish(human, "route-human")
    original_human = runtime.ctl("activewindow", True)["address"]
    # A root can connect to the shared primary socket and still be assigned
    # before publication by the per-invocation launch record.
    a, b = new_client("source-A"), new_client("source-B")
    for process, name in ((a, "A"), (b, "B")):
        action = context(name)
        runtime.ok("seat route-client " + action["token"] + " " + str(process.pid))
        # Change the source view after accepting the invocation.
        runtime.ok("seat workspace " + name + " name:after-" + name)
        window = publish(process, "route-root-" + name)
        assert window["workspace"]["name"] == "route-" + name, window
        assert runtime.ctl("activewindow", True)["address"] == original_human
        record(name + " cold root keeps accepted workspace and human focus", window)
        runtime.ok("seat workspace " + name + " name:route-" + name)
    # Same-connection parent before first buffer.
    command(a, "create route-same-parent"); command(a, "parent 1 0"); command(a, "map 1")
    same = wait(lambda: client("route-same-parent"))
    assert same["workspace"] == client("route-root-A")["workspace"]
    record("same-connection child first publishes with parent", same)
    handle = command(a, "export 0")
    service = new_client("shared-foreign")
    command(service, "import " + handle)
    command(service, "create route-foreign-before"); command(service, "foreign 0 0"); command(service, "map 0")
    child = wait(lambda: client("route-foreign-before"))
    assert child["workspace"] == client("route-root-A")["workspace"]
    record("cross-connection parent before first buffer", child)
    command(service, "create route-foreign-late"); command(service, "map 1")
    time.sleep(.3)
    assert not client("route-foreign-late")
    assert not any("route-foreign-late" in event for event in publications)
    command(service, "foreign 1 0")
    late = wait(lambda: client("route-foreign-late"))
    assert late["workspace"] == client("route-root-A")["workspace"]
    record("announced foreign producer holds first buffer until late parent", late)
    # Activation before first buffer persists its source instead of being lost.
    warm = new_client("warm-singleton")
    command(warm, "create route-premap-activation")
    action = context("B"); command(warm, "activate " + action["token"] + " 0"); command(warm, "map 0")
    window = wait(lambda: client("route-premap-activation"))
    assert window["workspace"]["name"] == "route-B", window
    assert runtime.ctl("activewindow", True)["address"] == original_human
    record("activation before map keeps B source", window)
    # An explicit parent wins over an unrelated token on the shared child.
    command(service, "activate " + context("B")["token"] + " 0")
    assert client("route-foreign-before")["workspace"]["name"] == "route-A"
    assert runtime.ctl("seat state A", True)["windowAddress"] == client("route-foreign-before")["address"]
    record("child activation uses parent A despite token from B", client("route-foreign-before"))
    # Re-route the parent through standard activation; all mapped transients follow.
    command(a, "activate " + context("B")["token"] + " 0")
    for title in ("route-root-A", "route-same-parent", "route-foreign-before", "route-foreign-late"):
        wait(lambda: client(title)["workspace"]["name"] == "route-B")
    assert runtime.ctl("seat state B", True)["windowAddress"] == client("route-root-A")["address"]
    record("moving parent updates same- and cross-connection descendants", [client(title) for title in ("route-root-A", "route-same-parent", "route-foreign-before")])
    # A real GTK3 file chooser, sharing the primary connection environment,
    # receives B's exported parent before it shows. Its folder is synthetic.
    gtk_flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "gtk+-3.0", "gdk-wayland-3.0"], text=True).split()
    subprocess.run(["cc", str(SOURCE / "routing-gtk.c"), "-o", str(runtime.base / "gtk-chooser"), *gtk_flags], check=True)
    folder = runtime.base / "empty-chooser"; folder.mkdir()
    gtk = runtime.start([str(runtime.base / "gtk-chooser"), command(b, "export 0"), str(folder), str(runtime.base / "gtk-entry")], "gtk-chooser")
    chooser = wait(lambda: client("route-gtk-chooser"))
    assert chooser["workspace"]["name"] == "route-B", chooser
    assert runtime.ctl("activewindow", True)["address"] == original_human
    record("real GTK3 chooser first maps with cross-connection parent B", chooser)
    driver = runtime.start([str(pathlib.Path(os.environ["MULTISEAT_TEST_DIR"]) / "input"), "B", "physical"],
                           "B-input", env=runtime.env | {"WAYLAND_DISPLAY": runtime.ctl("seat state B", True)["display"]},
                           stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert select.select([driver.stdout], [], [], 5)[0] and driver.stdout.readline().strip() == "ready"
    runtime.ok("seat focus B address:" + chooser["address"])
    command(driver, "type chooserB")
    wait(lambda: (runtime.base / "gtk-entry").read_text() == "chooserB")
    assert runtime.ctl("activewindow", True)["address"] == original_human
    record("B can type into the shared GTK chooser without changing human focus", "chooserB")
    command(a, "destroy 0")
    assert client("route-foreign-before")["workspace"]["name"] == "route-B"
    record("parent close retains child's last explicit route", client("route-foreign-before"))
    # Bound the guarantee: a producer announcing nothing before map is an
    # ordinary root. A later parent can repair ownership but cannot undo its
    # already-published frame. Keep this negative control in the evidence.
    unannounced = new_client("unannounced")
    first = publish(unannounced, "route-unannounced")
    assert first["workspace"]["name"] == "1"
    command(b, "export 0"); handle_b = command(b, "export 0")
    command(unannounced, "import " + handle_b); command(unannounced, "foreign 0 0")
    wait(lambda: client("route-unannounced")["workspace"]["name"] == "route-B")
    record("negative control: unannounced post-map parent cannot promise zero first frame", {"first": first, "final": client("route-unannounced")})
    # An explicit primary-Shell action must not inherit a currently presented
    # Agent view. Ordinary physical scopes keep their pre-existing semantics.
    owner = "routing-primary-source-owner-0000000000000001"
    state_b = runtime.ctl("seat state B", True)
    presentation = runtime.ctl("seat present B " + state_b["seatId"] + " physical current " + owner, True)
    assert presentation["active"], presentation
    source = runtime.ctl("seat route-context main - - 1 physical", True)
    primary_in_view = new_client("explicit-main-in-view")
    runtime.ok("seat route-client " + source["token"] + " " + str(primary_in_view.pid))
    window = publish(primary_in_view, "route-primary-in-view")
    assert window["workspace"]["name"] == "1", window
    runtime.ok("seat unpresent " + owner)
    record("explicit main source stays primary while an Agent presentation is active", window)
    router_verify = os.environ.get("CORNICE_ROUTER_VERIFY")
    if router_verify:
        router = runtime.start([router_verify], "native-router-e2e", env=runtime.env | {"CORNICE_ROUTE_E2E_CLIENT": str(runtime.base / "client")})
        assert router.wait(timeout=40) == 0, (runtime.base / "native-router-e2e.log").read_text()
        record("native router + private SNI + real Wayland H/A/B cold and warm activation", (runtime.base / "native-router-e2e.log").read_text())
    runtime.ok("output create headless physical-second")
    runtime.ok('eval hl.monitor({output="physical-second",mode="1280x800",position="1280x0",scale=1})')
    runtime.ok('dispatch hl.dsp.focus({monitor="physical-second"})')
    runtime.ok('dispatch hl.dsp.focus({workspace="2"})')
    runtime.ok('dispatch hl.dsp.focus({monitor="physical"})')
    second_output = new_client("second-primary-output")
    action = runtime.ctl("seat route-context main - - 2 physical-second", True)
    runtime.ok("seat route-client " + action["token"] + " " + str(second_output.pid))
    primary_second = publish(second_output, "route-primary-second")
    assert primary_second["workspace"]["name"] == "2", primary_second
    assert runtime.ctl("seat route-context main - - 2 absent-output") == "source output unavailable"
    assert runtime.ctl("seat route-context main - - missing-workspace physical-second") == "source workspace changed"
    record("primary action uses its explicit output and rejects stale/missing sources", primary_second)
    time.sleep(.2)
    for title in ("route-root-A", "route-root-B", "route-same-parent", "route-foreign-before", "route-foreign-late", "route-premap-activation"):
        events = [event for event in publications if event.startswith("openwindow>>") and event.endswith("," + title)]
        assert len(events) == 1 and ",route-" in events[0], (title, events)
    record("all supported first publications name the correct explicit workspace", publications)
finally:
    stop_events.set()
    runtime.cleanup()
