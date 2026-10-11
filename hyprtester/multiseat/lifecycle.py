"""Native keyboard, input-client revocation, lock and socket lifecycles."""
import json
import os
import pathlib
import select
import signal
import subprocess
from runtime import Runtime, wait

FIXTURES = pathlib.Path(os.environ["MULTISEAT_TEST_DIR"])
SOURCE = pathlib.Path(__file__).resolve().parent
RESULTS = []


def record(name, detail):
    RESULTS.append({"test": name, "observed": detail})
    print("PASS", name, flush=True)


def line(process, expected=None):
    assert select.select([process.stdout], [], [], 5)[0], "fixture response timeout"
    answer = process.stdout.readline().strip()
    if expected is not None:
        assert answer == expected, (answer, expected)
    return answer


def send(process, command):
    process.stdin.write(command + "\n")
    process.stdin.flush()
    return line(process)


def device(runtime, empty=False):
    process = runtime.start([str(FIXTURES / "input"), "Hyprland", "physical"], "input", env=runtime.env | ({"MULTISEAT_EMPTY_KEYMAP": "1"} if empty else {}),
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    line(process, "ready")
    return process


def locker(runtime, outputs="physical", keep_denied=False, display=None):
    environment = runtime.env | {"MULTISEAT_OUTPUT": outputs} | ({"MULTISEAT_KEEP_DENIED_LOCK": "1"} if keep_denied else {})
    if display is not None:
        environment["WAYLAND_DISPLAY"] = display
    return runtime.start([str(FIXTURES / "lock")], "ordinary-lock", env=environment,
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)


def locked(runtime):
    return wait(lambda: runtime.ctl("seat lock-state", True)["secure"])


def registry(runtime, display, label):
    probe = runtime.start([str(FIXTURES / "registry")], "registry-" + label,
                          env=runtime.env | {"WAYLAND_DISPLAY": display}, stdout=subprocess.PIPE, text=True)
    output, _ = probe.communicate(timeout=5)
    assert probe.returncode == 0, (label, probe.returncode, output)
    return output.splitlines()


def management_display(runtime, secondary):
    assert "management-display-v1" in runtime.ctl("seat capabilities", True)["features"]
    ordinary = registry(runtime, runtime.env["WAYLAND_DISPLAY"], "ordinary-before")
    assert "output_name=physical" in ordinary and "output_name=private-view" not in ordinary, ordinary
    display = runtime.ctl("seat management-display", True)["display"]
    assert (runtime.rt / display).is_socket(), display
    assert runtime.ctl("seat management-display", True)["display"] == display, "management socket identity changed"
    management = registry(runtime, display, "management")
    preferred = registry(runtime, secondary, "preferred")
    after = registry(runtime, runtime.env["WAYLAND_DISPLAY"], "ordinary-after")
    assert "output_name=physical" in management and "output_name=private-view" in management, management
    assert "output_name=private-view" in preferred, preferred
    assert "output_name=private-view" not in after, "requesting management display widened ordinary registry visibility"
    scope_global = "global=hyprland_lock_scope_manager_v1 version=1"
    assert scope_global in management and scope_global in after and scope_global not in preferred, (management, after, preferred)
    assert "global=ext_session_lock_manager_v1 version=1" in management, management
    assert management.count("seat_name=Hyprland") == 1, "management socket exposed a preferred-seat alias"
    record("explicit management registry preserves ordinary private-output filtering",
           {"ordinaryPrivateVisible": False, "managementPrivateVisible": True, "preferredScopeVisible": False})
    return display


def controlled_device(runtime, name, state, shared=False, delayed=False):
    display = runtime.env["WAYLAND_DISPLAY"] if shared else state["display"]
    environment = runtime.env | {"WAYLAND_DISPLAY": display}
    if delayed:
        environment["MULTISEAT_DELAY_DEVICES"] = "1"
    process = runtime.start([str(FIXTURES / "input"), state.get("seatName", state["name"]), "physical"], name, env=environment,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    line(process, "ready")
    assert process.poll() is None
    return process


def control(runtime, name, action, owner=None):
    state = runtime.ctl("seat state " + name, True)
    command = "seat control " + name + " " + state["seatId"] + " " + state["generation"] + " " + action
    if owner is not None:
        command += " " + str(owner)
    evidence = {"command": command, "state": state,
                "monitors": runtime.ctl("monitors", True), "lock": runtime.ctl("seat lock-state", True)}
    artifact = runtime.base / "last-control.json"
    artifact.write_text(json.dumps(evidence, indent=2))
    try:
        result = runtime.ctl(command, True)
    except RuntimeError as error:
        evidence["error"] = str(error)
        artifact.write_text(json.dumps(evidence, indent=2))
        raise
    evidence["result"] = result
    artifact.write_text(json.dumps(evidence, indent=2))
    assert result["paused"] == (action == "pause"), (command, result)
    assert result["generation"] != state["generation"], (state, result)
    return result


def malformed_primary_owners(runtime):
    state = runtime.ctl("seat state main", True)
    assert state["paused"], "malformed owner requests must start from a paused primary seat"
    results = []
    for owner in ("0", "-1", "invalid", "123x", "2147483648"):
        request = "seat control main " + state["seatId"] + " " + state["generation"] + " resume-composed " + owner
        answer = runtime.ctl(request)
        after = runtime.ctl("seat state main", True)
        results.append({"owner": owner, "response": answer, "before": state, "after": after})
        (runtime.base / "primary-owner-rejections.json").write_text(json.dumps(results, indent=2))
        assert answer == "primary control unavailable", (request, answer)
        for field in ("paused", "generation"):
            assert after[field] == state[field], (owner, field, state, after)
    record("malformed primary input owner is rejected without changing authorization", [item["owner"] for item in results])


def create_devices(process):
    for command in ("new-pointer", "new-keyboard"):
        assert send(process, command) == "done"


def primary_state(runtime):
    return {"workspace": runtime.ctl("activeworkspace", True),
            "window": runtime.ctl("activewindow", True),
            "cursor": runtime.ctl("cursorpos", True),
            "outputs": [(m["name"], m["activeWorkspace"]) for m in runtime.ctl("monitors", True)]}


def focus_entry(runtime, name, application, target, driver, offset=0):
    window = wait(lambda: next((w for w in runtime.ctl("clients", True) if w["pid"] == application.pid), None))
    state = runtime.ctl("seat state " + name, True)
    assert window["workspace"]["name"] == state["workspace"], (window, state)
    if name == "main":
        runtime.ok('dispatch hl.dsp.focus({window="pid:' + str(application.pid) + '"})')
    else:
        runtime.ok("seat focus " + name + " address:" + window["address"])
    allocation = wait(lambda: json.loads(pathlib.Path(str(target) + ".geometry").read_text()))["entry"]
    x = round(window["at"][0] - state["position"][0] + allocation[0] + allocation[2] / 2) + offset
    y = round(window["at"][1] - state["position"][1] + allocation[1] + allocation[3] / 2)
    for command in (f"motion {x} {y}", "button 272 1", "button 272 0"):
        assert send(driver, command) == "done"
    wait(lambda: pathlib.Path(str(target) + ".ready").read_text() == "focused")
    return x, y


def revoked_input(runtime, name, driver, target):
    before = runtime.ctl("seat state " + name, True)
    assert not before["paused"], before
    assert target.read_text() == "before"
    # Both resources are created after resume on a real connection whose
    # registry/seat roundtrip predates revocation. No prior device exists in
    # secondary cases; the primary case explicitly destroyed its old devices.
    create_devices(driver)
    for command in ("type stale", "motion 19 23", "relative 7 9"):
        assert send(driver, command) == "done"
    after = runtime.ctl("seat state " + name, True)
    for field in ("cursor", "windowId", "workspace", "output", "generation", "paused"):
        assert after[field] == before[field], (name, field, before, after)
    assert target.read_text() == "before", "revoked keyboard input reached the GTK entry"
    assert driver.poll() is None, "denied input must not kill the unrelated Wayland connection"


def late_input_cases(runtime, shared=False, primary=False, recreate=True):
    name = "main" if primary else "research"
    if not primary:
        runtime.ok("seat create research physical")
        runtime.ok("seat workspace research 10")
    state = runtime.ctl("seat state " + name, True)
    target = runtime.base / "revocation-text"
    application = runtime.start([os.environ["HYPRLAND_TEST_PYTHON"], str(SOURCE / "gtk-window.py"), "revocation-target", str(target)],
                                "revocation-app", env=runtime.env | {"WAYLAND_DISPLAY": state["display"]})
    wait(lambda: any(w["pid"] == application.pid for w in runtime.ctl("clients", True)))
    old = controlled_device(runtime, "old-connection", state, shared=shared, delayed=True)
    owner = old.pid
    if primary and recreate:
        malformed_primary_owners(runtime)
        # Grant uses real credentials, then establish a fresh client after its
        # cutoff. This PID must remain the same through both reconnects.
        control(runtime, name, "resume-composed", owner)
        assert send(old, "reconnect") == "done"
        create_devices(old)
        baseline = old
    else:
        baseline = controlled_device(runtime, "baseline-input", state, shared=shared)
    focus_entry(runtime, name, application, target, baseline)
    assert send(baseline, "type before") == "done"
    wait(lambda: target.read_text() == "before")
    if primary and not recreate:
        # Native baseline input predates authorization. The authorized process
        # opens a new connection but leaves it entirely without devices across
        # pause/resume, so revocation cannot depend on an existing device lease.
        runtime.stop(baseline)
        control(runtime, name, "resume-composed", owner)
        assert send(old, "reconnect") == "done"
    original_primary = primary_state(runtime)
    control(runtime, name, "pause")
    resumed = control(runtime, name, "resume-composed" if primary else "resume", owner if primary else None)
    if primary and recreate:
        assert send(old, "destroy-devices") == "done"
    revoked_input(runtime, name, old, target)
    if not primary:
        assert primary_state(runtime) == original_primary, "secondary late devices changed physical desktop state"
        fresh = controlled_device(runtime, "fresh-connection", resumed, shared=shared)
    else:
        assert send(old, "reconnect") == "done"
        assert old.pid == owner and old.poll() is None
        create_devices(old)
        fresh = old
    # Positive control uses the same application and actual protocol path.
    # Its final exact text also fences asynchronous GTK delivery: any stale
    # text queued before the fresh input would make this expectation fail.
    position = focus_entry(runtime, name, application, target, fresh, offset=7)
    actual = runtime.ctl("seat state " + name, True)["cursor"]
    assert all(abs(actual[index] - position[index]) < 1 for index in (0, 1)), (position, actual)
    assert send(fresh, "type fresh") == "done"
    wait(lambda: target.read_text() == "beforefresh")
    assert target.read_text() == "beforefresh", "old source injected text before the fresh connection"
    assert application.poll() is None and fresh.poll() is None
    assert not runtime.ctl("seat state " + name, True)["paused"]
    if not primary:
        assert primary_state(runtime) == original_primary, "fresh secondary input changed physical desktop state"
    return {"socket": "default-shared" if shared else "preferred", "samePidReconnect": primary,
            "oldDevices": "recreated" if primary and recreate else "first-created",
            "oldSourceBlocked": True, "freshSourceTyped": target.read_text(), "primaryUnchanged": not primary}


# Use actual empty xkb layout and raw text JSON, not a synthetic IPC response.
runtime = Runtime("empty-layout")
try:
    runtime.launch()
    driver = device(runtime, empty=True)
    devices = wait(lambda: runtime.ctl("devices", True)["keyboards"])
    assert any(keyboard["active_layout_index"] is None and keyboard["active_keymap"] == "none" for keyboard in devices), devices
    assert "none" in runtime.ctl("devices"), runtime.ctl("devices")
    record("empty keyboard layout remains valid JSON", devices)
finally:
    runtime.cleanup()


for before in (False, True):
    runtime = Runtime("cold-layer-before-keyboard" if before else "cold-keyboard-before-layer")
    try:
        runtime.launch()
        target = runtime.base / "underlying-text"
        runtime.start([os.environ["HYPRLAND_TEST_PYTHON"], str(SOURCE / "gtk-window.py"), "underlying-native", str(target)], "application")
        wait(lambda: any(window["title"] == "underlying-native" for window in runtime.ctl("clients", True)))
        if not before:
            driver = device(runtime)
        panel = runtime.start([str(FIXTURES / "layer")], "exclusive-panel", stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        line(panel, "ready")
        if before:
            driver = device(runtime)
        wait(lambda: int(send(panel, "enters")) > 0)
        assert send(driver, "key 28 1") == "done"
        assert send(driver, "key 28 0") == "done"
        assert int(send(panel, "keys")) == 1, "first key was not delivered exactly once to exclusive layer"
        # Enter alone cannot alter Gtk.Entry text. Follow it with printable e
        # to prove that duplicate or misrouted ordinary input cannot leak.
        assert send(driver, "key 18 1") == "done"
        assert send(driver, "key 18 0") == "done"
        assert int(send(panel, "keys")) == 2, "printable key did not stay in exclusive layer"
        assert not target.exists() or target.read_text() == "", "exclusive input leaked to underlying GTK client"
        record("exclusive layer gets first cold keyboard key", {"panelBeforeKeyboard": before})
    finally:
        runtime.cleanup()

runtime = Runtime("cold-full-lock")
try:
    runtime.launch()
    lock = locker(runtime)
    line(lock, "ready")
    locked(runtime)
    driver = device(runtime)
    assert send(driver, "key 28 1") == "done"
    assert send(driver, "key 28 0") == "done"
    assert int(send(lock, "keys")) == 1, "first cold keyboard key bypassed full lock"
    assert runtime.ctl("seat lock-state", True)["locked"]
    record("full lock receives first cold keyboard key", {"stillSecure": True})
finally:
    runtime.cleanup()

# All four combinations retain the pre-existing ordinary-lock config meaning.
for restore in (False, True):
    for orphan in (False, True):
        runtime = Runtime("ordinary-restore-" + str(restore) + "-orphan-" + str(orphan))
        try:
            runtime.launch()
            runtime.ok("eval hl.config({misc={allow_session_lock_restore=" + str(restore).lower() + "}})")
            first = locker(runtime, keep_denied=restore and not orphan)
            line(first, "ready")
            locked(runtime)
            old = runtime.ctl("seat lock-state", True)
            if orphan:
                runtime.stop(first, signal.SIGKILL)
                wait(lambda: not runtime.ctl("seat lock-state", True)["ownerConnected"])
            replacement = locker(runtime)
            if restore:
                line(replacement, "ready")
                locked(runtime)
                current = runtime.ctl("seat lock-state", True)
                assert current["scope"] == "session" and current["ownerConnected"] and current["lockId"] != old["lockId"], current
                if not orphan:
                    # A finished old owner retains its actual protocol objects
                    # and attempts stale unlock/destroy against the secure new
                    # owner. These requests must not release the current lock.
                    line(first, "finished")
                    assert send(first, "unlock") == "done"
                    assert first.wait(timeout=5) == 0
                    after_stale_owner = runtime.ctl("seat lock-state", True)
                    assert after_stale_owner["locked"] and after_stale_owner["secure"] and after_stale_owner["ownerConnected"], after_stale_owner
                    assert after_stale_owner["lockId"] == current["lockId"], after_stale_owner
                assert send(replacement, "unlock") == "done"
                wait(lambda: not runtime.ctl("seat lock-state", True)["locked"])
            else:
                assert replacement.wait(timeout=5) == 3, "restore=false must deny ordinary replacement"
                current = runtime.ctl("seat lock-state", True)
                assert current["locked"] and current["scope"] == "session" and current["lockId"] == old["lockId"], current
                if not orphan:
                    assert first.poll() is None
                    assert send(first, "unlock") == "done"
                    wait(lambda: not runtime.ctl("seat lock-state", True)["locked"])
            record("ordinary lock restore compatibility", {"allowRestore": restore, "ownerLost": orphan})
        finally:
            runtime.cleanup()

# Scoped to ordinary is a full security upgrade even when ordinary restore is off.
runtime = Runtime("scoped-full-upgrade")
try:
    runtime.launch()
    runtime.ok("eval hl.config({misc={allow_session_lock_restore=false}})")
    runtime.ok("seat create-private-output private-view")
    wait(lambda: any(monitor["name"] == "private-view" for monitor in runtime.ctl("monitors", True)))
    runtime.ok("seat create research private-view")
    runtime.ok("seat workspace research 10")
    seat = runtime.ctl("seat state research", True)
    management = management_display(runtime, seat["display"])
    result = runtime.ctl("seat lock-policy research " + seat["seatId"] + " " + seat["generation"] + " allow", True)
    assert result["scopePolicy"] == "allow", result
    scope = runtime.start([str(FIXTURES / "scope-probe"), "research", seat["seatId"], seat["generation"], "private-view"], "scoped-lock", env=runtime.env | {"WAYLAND_DISPLAY": management}, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, bufsize=1)
    line(scope, "secure")
    locked(runtime)
    protected = runtime.ctl("seat lock-state", True)
    assert protected["scope"] == "scoped" and {output["name"] for output in protected["protectedOutputs"]} == {"physical"}, protected
    # The ordinary fixture presents real lock frames on both outputs.
    full = locker(runtime, "physical,private-view", display=management)
    wait(lambda: runtime.ctl("seat lock-state", True)["scope"] == "session")
    upgraded = runtime.ctl("seat lock-state", True)
    assert upgraded["scope"] == "session" and upgraded["locked"], upgraded
    assert {output["name"] for output in upgraded["protectedOutputs"]} == {"physical", "private-view"}, upgraded
    line(full, "ready")
    locked(runtime)
    line(scope, "finished")
    current = runtime.ctl("seat lock-state", True)
    assert current["secure"] and all(output["covered"] for output in current["protectedOutputs"]), current
    # The previous scoped owner still has live protocol objects. Its stale
    # unlock/destroy request cannot release the complete replacement lock.
    scope.stdin.write("unlock\n"); scope.stdin.flush()
    assert scope.wait(timeout=5) == 0
    after_stale_owner = runtime.ctl("seat lock-state", True)
    assert after_stale_owner["secure"] and after_stale_owner["locked"] and after_stale_owner["ownerConnected"], after_stale_owner
    assert after_stale_owner["lockId"] == current["lockId"], after_stale_owner
    assert runtime.ctl("seat state research", True)["paused"]
    assert send(full, "unlock") == "done"
    wait(lambda: not runtime.ctl("seat lock-state", True)["locked"])
    record("scoped lock upgrade revokes exemptions with restore=false", {"fullSecure": True})
finally:
    runtime.cleanup()

# Losing either independent scope owner or guard removes exemptions and
# requires an authenticated complete-session owner before recovery can unlock.
for loss in ("owner", "guardian"):
    runtime = Runtime("scoped-loss-" + loss)
    try:
        runtime.launch()
        runtime.ok("eval hl.config({misc={allow_session_lock_restore=true}})")
        runtime.ok("seat create-private-output private-view")
        wait(lambda: any(m["name"] == "private-view" for m in runtime.ctl("monitors", True)))
        runtime.ok("seat create research private-view")
        runtime.ok("seat workspace research 10")
        seat = runtime.ctl("seat state research", True)
        management = management_display(runtime, seat["display"])
        runtime.ctl("seat lock-policy research " + seat["seatId"] + " " + seat["generation"] + " allow", True)
        guard = runtime.start([str(FIXTURES / "lock-guard")], "independent-guardian", env=runtime.env | {"WAYLAND_DISPLAY": management}, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        line(guard, "ready")
        scope = runtime.start([str(FIXTURES / "scope-probe"), "research", seat["seatId"], seat["generation"], "private-view"], "scoped-lock", env=runtime.env | {"WAYLAND_DISPLAY": management}, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        line(scope, "secure")
        locked(runtime)
        assert runtime.ctl("seat lock-state", True)["scope"] == "scoped"
        runtime.stop(scope if loss == "owner" else guard, signal.SIGKILL)
        wait(lambda: runtime.ctl("seat lock-state", True)["scope"] == "session" and not runtime.ctl("seat lock-state", True)["ownerConnected"])
        orphaned = runtime.ctl("seat lock-state", True)
        assert orphaned["locked"] and orphaned["phase"] == "orphaned", orphaned
        assert {output["name"] for output in orphaned["protectedOutputs"]} == {"physical", "private-view"}, orphaned
        assert runtime.ctl("seat state research", True)["paused"]
        full = locker(runtime, "physical,private-view", display=management)
        line(full, "ready")
        locked(runtime)
        restored = runtime.ctl("seat lock-state", True)
        assert restored["scope"] == "session" and restored["ownerConnected"] and all(output["covered"] for output in restored["protectedOutputs"]), restored
        assert send(full, "unlock") == "done"
        wait(lambda: not runtime.ctl("seat lock-state", True)["locked"])
        assert runtime.ctl("seat state research", True)["paused"], "recovery must not resume revoked control"
        record("scoped loss escalates and requires complete-lock recovery", {"lost": loss})
    finally:
        runtime.cleanup()

# These cases hold actual live clients across pause/resume, rather than only
# testing devices whose generation was captured before the transition.
for name, shared, primary, recreate in (("preferred-late-device", False, False, False),
                                        ("shared-late-device", True, False, False),
                                        ("primary-same-pid-device-recreation", True, True, True),
                                        ("primary-same-pid-late-device", True, True, False)):
    runtime = Runtime(name)
    try:
        runtime.launch()
        record("old client cannot regain input by creating devices", late_input_cases(runtime, shared=shared, primary=primary, recreate=recreate))
    finally:
        runtime.cleanup()

runtime = Runtime("socket-recreation")
try:
    runtime.launch()
    runtime.ok("seat create research physical")
    old = runtime.ctl("seat state research", True)
    old_socket = runtime.rt / old["display"]
    assert old_socket.is_socket(), old
    runtime.stop(runtime.compositor, signal.SIGKILL)
    assert old_socket.is_socket(), "crash fixture must retain the stale socket node"
    runtime.launch()
    runtime.ok("seat create research physical")
    current = runtime.ctl("seat state research", True)
    assert current["display"] != old["display"] and current["seatId"] != old["seatId"], (old, current)
    assert old_socket.is_socket() and (runtime.rt / current["display"]).is_socket()
    application = runtime.start([os.environ["HYPRLAND_TEST_PYTHON"], str(SOURCE / "gtk-window.py"), "recreated-native", str(runtime.base / "recreated-text")], "recreated-app", env=runtime.env | {"WAYLAND_DISPLAY": current["display"]})
    wait(lambda: any(window["title"] == "recreated-native" for window in runtime.ctl("clients", True)))
    assert application.poll() is None
    record("crashed compositor recreates seat with a new live socket", {"old": old["display"], "new": current["display"]})
finally:
    runtime.cleanup()

(FIXTURES / "lifecycle-results.json").write_text(json.dumps(RESULTS, indent=2) + "\n")
