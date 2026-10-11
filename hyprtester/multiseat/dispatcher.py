"""Standalone real seat-dispatch regression cases; import from test runtime.

Call workspace_cases(runtime) after Runtime.launch().
The Runtime fixture and this script must run through the isolated suite wrapper.
"""
import json
import os
import pathlib
import select
import subprocess
import sys


def primary_state(runtime):
    return {"workspace": runtime.ctl("activeworkspace", True),
            "window": runtime.ctl("activewindow", True),
            "cursor": runtime.ctl("cursorpos", True),
            "outputs": [(m["name"], m["activeWorkspace"]) for m in runtime.ctl("monitors", True)]}


def seat_dispatch(runtime, name, expression):
    seat = runtime.ctl("seat state " + name, True)
    runtime.ok("seat dispatch " + name + " " + seat["seatId"] + " " + str(seat["generation"]) + " " + expression)


def workspace_cases(runtime):
    # Hold real numbered workspace objects, avoiding accidental collection of
    # empty workspaces while testing existing-workspace monitor selectors.
    runtime.ok("output create headless remote")
    runtime.ok('eval hl.monitor({output="remote",mode="1280x800",position="1280x0",scale=1})')
    runtime.ok('eval hl.config({binds={workspace_back_and_forth=0}})')
    runtime.ok('dispatch hl.dsp.focus({workspace="7"})')
    original = primary_state(runtime)
    for number, output in ((110, "physical"), (111, "physical"), (112, "physical"), (201, "remote"), (202, "remote")):
        name = "holder-" + str(number)
        runtime.ok("seat create " + name + " " + output)
        runtime.ok("seat workspace " + name + " " + str(number))
    runtime.ok("seat create dispatcher physical")
    runtime.ok("seat workspace dispatcher 110")

    def focus(selector, expected):
        seat_dispatch(runtime, "dispatcher", 'hl.dsp.focus({workspace=' + json.dumps(selector) + '})')
        current = runtime.ctl("seat state dispatcher", True)
        assert current["workspace"] == str(expected), (selector, expected, current)
        assert primary_state(runtime) == original, (selector, original, primary_state(runtime))

    focus("+1", 111)
    focus("+1", 112)
    focus("-1", 111)
    focus("previous", 112)
    focus("previous", 111)
    focus("m+1", 112)
    focus("201", 201)
    focus("m+1", 202)
    focus("m-1", 201)
    focus("previous_per_monitor", 202)

    # Mode 1 follows the acting seat's cross-output MRU; mode 2 restricts it
    # to the acting seat's current output, never the primary MRU.
    focus("110", 110)
    focus("201", 201)
    focus("111", 111)
    runtime.ok('eval hl.config({binds={workspace_back_and_forth=1}})')
    focus("111", 201)
    runtime.ok('eval hl.config({binds={workspace_back_and_forth=0}})')
    focus("110", 110)
    focus("201", 201)
    focus("111", 111)
    runtime.ok('eval hl.config({binds={workspace_back_and_forth=2}})')
    focus("111", 110)
    runtime.ok('eval hl.config({binds={workspace_back_and_forth=0}})')

    # Empty-workspace directional navigation uses the acting seat's monitor
    # fallback and records its own history without changing physical state.
    focus("110", 110)
    runtime.ok('eval hl.config({binds={window_direction_monitor_fallback=1}})')
    remote = next(m for m in runtime.ctl("monitors", True) if m["name"] == "remote")["activeWorkspace"]["name"]
    seat_dispatch(runtime, "dispatcher", 'hl.dsp.focus({direction="right"})')
    fallback = runtime.ctl("seat state dispatcher", True)
    assert fallback["output"] == "remote" and fallback["workspace"] == remote, (remote, fallback)
    assert primary_state(runtime) == original
    focus("previous", 110)
    return {"monitorFallback": True, "relative": True, "monitorRelative": True, "privateHistory": True, "backAndForthModes": [1, 2]}


def fixture_line(process, expected=None):
    assert select.select([process.stdout], [], [], 5)[0], "fixture response timeout"
    value = process.stdout.readline().strip()
    if expected is not None:
        assert value == expected, (expected, value)
    return value


def fixture_send(process, command):
    process.stdin.write(command + "\n")
    process.stdin.flush()
    return fixture_line(process)


def constraint_cases(runtime, fixture_dir, source_dir, wait):
    """Primary constraint must survive native secondary focus navigation.

    Uses real lock/unlock protocol events in constraints fixture stderr. The
    passed wait helper must await predicates rather than introducing sleeps.
    """
    fixture_dir = pathlib.Path(fixture_dir)
    source_dir = pathlib.Path(source_dir)
    primary = runtime.start([str(fixture_dir / "input"), "Hyprland", "physical"], "primary-input",
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    fixture_line(primary, "ready")
    primary_client = runtime.start([str(fixture_dir / "constraints")], "primary-constraint", stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)
    fixture_line(primary_client, "ready")
    primary_window = wait(lambda: next((w for w in runtime.ctl("clients", True) if w["pid"] == primary_client.pid), None))
    px = primary_window["at"][0] + primary_window["size"][0] // 2
    py = primary_window["at"][1] + primary_window["size"][1] // 2
    runtime.ok('dispatch hl.dsp.focus({window="pid:' + str(primary_client.pid) + '"})')
    # The first window can appear exactly under the initial cursor. Establish
    # a real surface enter before requesting a pointer constraint rather than
    # relying on a same-coordinate absolute event to refocus a cold pointer.
    for command in (f"motion {px + 7} {py + 5}", f"motion {px} {py}"):
        assert fixture_send(primary, command) == "done"
    wait(lambda: fixture_send(primary_client, "pointer-focused") == "1")
    for command in ("button 272 1", "button 272 0"):
        assert fixture_send(primary, command) == "done"
    assert fixture_send(primary_client, "lock") == "done"
    log = runtime.base / "primary-constraint.stderr"
    wait(lambda: "locked\n" in log.read_text())
    original = primary_state(runtime)

    runtime.ok("seat create navigation physical")
    runtime.ok("seat workspace navigation 10")
    current = runtime.ctl("seat state navigation", True)
    secondary = runtime.start([str(fixture_dir / "input"), "navigation", "physical"], "secondary-input",
                              env=runtime.env | {"WAYLAND_DISPLAY": current["display"]},
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    fixture_line(secondary, "ready")
    children = []
    for index in (1, 2):
        child = runtime.start([os.environ["HYPRLAND_TEST_PYTHON"], str(source_dir / "gtk-window.py"), "navigation-" + str(index), str(runtime.base / ("nav-text-" + str(index)))],
                              "navigation-app-" + str(index), env=runtime.env | {"WAYLAND_DISPLAY": current["display"]})
        children.append(child)
        wait(lambda: any(w["pid"] == child.pid for w in runtime.ctl("clients", True)))
    before = runtime.ctl("seat state navigation", True)["window"]
    # Native direction helper goes through switchToWindow, unlike the explicit
    # focus-selector early seat branch. Choose the geometry-consistent target.
    windows = [w for w in runtime.ctl("clients", True) if w["pid"] in {child.pid for child in children}]
    focused = next(w for w in windows if w["title"] == before)
    other = next(w for w in windows if w["title"] != before)
    dx = other["at"][0] - focused["at"][0]
    dy = other["at"][1] - focused["at"][1]
    direction = ("right" if dx > 0 else "left") if abs(dx) >= abs(dy) else ("down" if dy > 0 else "up")
    evidence = {"direction": direction, "windows": windows, "expected": other["title"],
                "before": runtime.ctl("seat state navigation", True), "primaryBefore": original}
    artifact = runtime.base / "navigation-focus.json"
    evidence["primaryPreDispatch"] = primary_state(runtime)
    evidence["clientsPreDispatch"] = runtime.ctl("clients", True)
    evidence["workspacesPreDispatch"] = runtime.ctl("workspaces", True)
    artifact.write_text(json.dumps(evidence, indent=2))
    assert evidence["primaryPreDispatch"] == original, ("secondary setup changed primary state", original, evidence["primaryPreDispatch"])
    seat_dispatch(runtime, "navigation", 'hl.dsp.focus({direction=' + json.dumps(direction) + '})')
    try:
        wait(lambda: runtime.ctl("seat state navigation", True)["window"] == other["title"])
    finally:
        evidence["after"] = runtime.ctl("seat state navigation", True)
        evidence["primaryAfter"] = primary_state(runtime)
        evidence["clientsAfter"] = runtime.ctl("clients", True)
        evidence["workspacesAfter"] = runtime.ctl("workspaces", True)
        artifact.write_text(json.dumps(evidence, indent=2))
    assert "unlocked\n" not in log.read_text(), log.read_text()
    assert evidence["primaryAfter"] == evidence["primaryPreDispatch"] == original, (original, evidence["primaryPreDispatch"], evidence["primaryAfter"])
    assert primary_state(runtime) == original, (original, primary_state(runtime))
    assert fixture_send(primary, "relative 30 10") == "done"
    assert runtime.ctl("cursorpos", True) == original["cursor"], "secondary action released primary cursor lock"
    assert "unlocked\n" not in log.read_text(), log.read_text()
    return {"primaryConstraintRetained": True, "primaryStateUnchanged": True, "secondaryNativeFocusChanged": True}


def shortcut_cases(runtime, fixture_dir, wait):
    fixture_dir = pathlib.Path(fixture_dir)
    runtime.ok("seat create research physical")
    runtime.ok("seat workspace research 1")
    seat = runtime.ctl("seat state research", True)
    primary = runtime.start([str(fixture_dir / "input"), "Hyprland", "physical"], "primary-input", stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    fixture_line(primary, "ready")
    secondary = runtime.start([str(fixture_dir / "input"), "research", "physical"], "secondary-input", env=runtime.env | {"WAYLAND_DISPLAY": seat["display"]},
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    fixture_line(secondary, "ready")
    application = runtime.start([str(fixture_dir / "constraints")], "shared-shortcut-client", env=runtime.env | {"MULTISEAT_OTHER_SEAT": "research"},
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    fixture_line(application, "ready")
    window = wait(lambda: next((w for w in runtime.ctl("clients", True) if w["pid"] == application.pid), None))
    runtime.ok('dispatch hl.dsp.focus({window="pid:' + str(application.pid) + '"})')
    runtime.ok('seat focus research address:' + window['address'])
    # A native Lua binding callback runs synchronously on the compositor input
    # path. The counter avoids racing asynchronous external command execution.
    runtime.ok('eval _G.multiseatShortcutCount = 0; hl.bind("SUPER + T", function() _G.multiseatShortcutCount = _G.multiseatShortcutCount + 1 end, {})')

    def count():
        return int(runtime.ctl("repl return tostring(_G.multiseatShortcutCount)"))

    def shortcut(device):
        for command in ("mods 64", "key 125 1", "key 20 1", "key 20 0", "key 125 0", "mods 0"):
            assert fixture_send(device, command) == "done"

    assert fixture_send(application, "inhibit") == "done"
    events = runtime.base / "shared-shortcut-client.stderr"
    wait(lambda: "shortcut-active-primary" in events.read_text())
    shortcut(primary)
    assert count() == 0, "requested primary seat binding was not inhibited"
    shortcut(secondary)
    assert count() == 1, "primary inhibitor blocked another seat sharing the surface"
    assert fixture_send(application, "inhibit-other") == "done"
    wait(lambda: "shortcut-active-other" in events.read_text())
    assert application.poll() is None, "different seats must legally inhibit the same surface"
    shortcut(primary)
    shortcut(secondary)
    assert count() == 1, "same-surface per-seat inhibitors did not independently block both bindings"
    assert fixture_send(application, "uninhibit") == "done"
    shortcut(primary)
    assert count() == 2, "destroying the primary inhibitor did not restore its native binding"
    assert application.poll() is None
    return {"oneSurfaceTwoSeats": True, "inhibitionOnlyOnRequestedSeat": True, "nativeBindingsRestoreOnDestroy": True}


if __name__ == "__main__":
    from runtime import Runtime, wait
    fixtures = pathlib.Path(os.environ["MULTISEAT_TEST_DIR"])
    source = pathlib.Path(__file__).resolve().parent
    results = []
    for name, callback in (("workspace", workspace_cases), ("constraint", lambda runtime: constraint_cases(runtime, fixtures, source, wait)), ("shortcut-inhibitor", lambda runtime: shortcut_cases(runtime, fixtures, wait))):
        runtime = Runtime("seat-dispatch-" + name)
        try:
            runtime.launch()
            result = callback(runtime)
            results.append({"test": name, "observed": result})
            print("PASS native seat dispatcher " + name, flush=True)
        finally:
            runtime.cleanup()
    (fixtures / "dispatcher-results.json").write_text(json.dumps(results, indent=2) + "\n")
