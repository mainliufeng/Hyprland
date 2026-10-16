"""Real Electron -> xdg-open -> Chrome singleton and focused token regressions.

All profiles, content, IPC, input and screenshots belong to the private Runtime.
No host app/profile, compositor socket, network or input device is available.
"""
import base64
import http.client
import json
import os
from pathlib import Path
import select
import subprocess
import time
from runtime import Runtime, wait
from activation_cdp import Cdp

SOURCE = Path(__file__).resolve().parent
runtime = Runtime("activation")
checks = []
cdp = None


def record(description, evidence):
    checks.append({"description": description, "evidence": evidence})
    (runtime.base / "activation.json").write_text(json.dumps(checks, indent=2))
    print("PASS", description, flush=True)


def command(process, value):
    process.stdin.write(value + "\n"); process.stdin.flush()
    assert select.select([process.stdout], [], [], 8)[0], value
    answer = process.stdout.readline().strip()
    assert answer.startswith("done"), (value, answer)
    return answer.removeprefix("done ")


def window(title):
    return next((w for w in runtime.ctl("clients", True) if w.get("mapped") and (w["title"] == title or w["title"].startswith(title + " - "))), None)


def focus_workspace(number):
    runtime.ok('dispatch hl.dsp.focus({workspace="' + str(number) + '"})')


def context(name="main"):
    if name == "main":
        workspace = runtime.ctl("activeworkspace", True)["name"]
        return runtime.ctl("seat route-context main - - " + workspace + " physical", True)
    state = runtime.ctl("seat state " + name, True)
    return runtime.ctl("seat route-context " + " ".join((name, state["seatId"], str(state["generation"]), state["workspace"])), True)


def protocol_client(label):
    p = runtime.start([str(runtime.base / "client")], label, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert select.select([p.stdout], [], [], 8)[0] and p.stdout.readline().strip() == "ready"
    return p


def evaluate(session, expression):
    return cdp.call("Runtime.evaluate", {"expression": expression, "returnByValue": True}, session)["result"].get("value")


try:
    runtime.env.update(XDG_CURRENT_DESKTOP="X-Generic", XDG_SESSION_TYPE="wayland", BROWSER="")
    runtime.launch()
    # Exercise existing configured focus behavior; this fix must preserve it.
    runtime.ok("eval hl.config({misc={focus_on_activate=true}})")
    profile = runtime.base / "chrome-profile"
    page = runtime.base / "initial.html"
    page.write_text("<title>Activation browser initial</title><h1>Private browser link test</h1>")
    apps = runtime.base / "data/applications"; apps.mkdir()
    desktop = apps / "activation-browser.desktop"
    desktop.write_text("[Desktop Entry]\nType=Application\nName=Activation private browser\nExec=/opt/google/chrome/chrome --no-sandbox --password-store=basic --ozone-platform=wayland --no-first-run --no-default-browser-check --disable-background-networking --no-proxy-server --user-data-dir=" + str(profile) + " %U\nMimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;\n")
    (runtime.base / "config/mimeapps.list").write_text("[Default Applications]\nx-scheme-handler/http=activation-browser.desktop\nx-scheme-handler/https=activation-browser.desktop\ntext/html=activation-browser.desktop\n")
    browser = runtime.start(["/opt/google/chrome/chrome", "--no-sandbox", "--password-store=basic", "--ozone-platform=wayland", "--no-first-run", "--no-default-browser-check", "--disable-background-networking", "--no-proxy-server", "--disable-component-update", "--disable-sync", "--disable-dev-shm-usage", "--disable-gpu", "--remote-debugging-port=0", "--user-data-dir=" + str(profile), page.as_uri()], "chrome")
    initial = wait(lambda: window("Activation browser initial"))
    address, pid = initial["address"], initial["pid"]
    wait(lambda: (profile / "DevToolsActivePort").exists())
    port = (profile / "DevToolsActivePort").read_text().splitlines()[0]
    cdp = Cdp("http://127.0.0.1:" + port)
    # A loopback HTTP server in the unshared network namespace proves actual
    # page loading, independently of compositor title/activation observations.
    runtime.start(["python3", "-m", "http.server", "8765", "--bind", "127.0.0.1", "--directory", str(runtime.base)], "http")
    def http_ready():
        c = http.client.HTTPConnection("127.0.0.1", 8765, timeout=2)
        c.request("GET", "/initial.html"); response = c.getresponse()
        return response.status == 200 and b"Activation browser initial" in response.read()
    wait(http_ready)
    focus_workspace(2)
    app = runtime.base / "electron.cjs"
    html = "<title>Activation source</title><h1>Open external link</h1><button autofocus>Open link</button><script>document.querySelector('button').onclick=()=>require('electron').ipcRenderer.send('open')</script>"
    app.write_text("const {app,BrowserWindow,ipcMain,shell}=require('electron');\n"
                   "const fs=require('fs'),path=require('path');\n"
                   "app.whenReady().then(()=>{const w=new BrowserWindow({webPreferences:{nodeIntegration:true,contextIsolation:false}});\n"
                   "w.loadURL(" + json.dumps("data:text/html," + html) + ");\n"
                   "ipcMain.on('open',async()=>{try{await shell.openExternal(fs.readFileSync(path.join(__dirname,'next-url.txt'),'utf8'));"
                   "fs.appendFileSync(path.join(__dirname,'opened.txt'),'opened\\n');}catch(e){fs.writeFileSync(path.join(__dirname,'error.txt'),String(e));}});});\n")
    runtime.start(["electron38", "--no-sandbox", "--password-store=basic", "--ozone-platform=wayland", "--user-data-dir=" + str(runtime.base / "electron-profile"), str(app)], "electron")
    source = wait(lambda: window("Activation source"))
    driver = runtime.start([str(Path(os.environ["MULTISEAT_TEST_DIR"]) / "input"), "Hyprland", "physical"], "input", stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    assert select.select([driver.stdout], [], [], 8)[0] and driver.stdout.readline().strip() == "ready"

    def click_link(label, workspace, focus_enabled):
        target = runtime.base / (label + ".html")
        target.write_text("<title>" + label + "</title><h1>Link loaded: " + label + "</h1>")
        url = "http://127.0.0.1:8765/" + target.name
        (runtime.base / "next-url.txt").write_text(url)
        focus_workspace(workspace)
        # Move only this test's source window when arranging same-workspace case.
        runtime.ok('dispatch hl.dsp.window.move({follow=false,workspace="' + str(workspace) + '",window="address:' + source["address"] + '"})')
        runtime.ok('dispatch hl.dsp.focus({window="address:' + source["address"] + '"})')
        command(driver, "key 28 1"); command(driver, "key 28 0")
        info = wait(lambda: next((t for t in cdp.call("Target.getTargets")["targetInfos"] if t.get("url") == url), None))
        session = cdp.call("Target.attachToTarget", {"targetId": info["targetId"], "flatten": True})["sessionId"]
        cdp.call("Network.enable", {}, session)
        try:
            wait(lambda: evaluate(session, "document.title") == label)
        except Exception:
            (runtime.base / "load-error.json").write_text(json.dumps({"target": info, "history": cdp.call("Page.getNavigationHistory", {}, session), "document": evaluate(session, "JSON.stringify({url:location.href,title:document.title,ready:document.readyState,body:document.body?.innerText})"), "events": cdp.events}, indent=2))
            raise
        actual = wait(lambda: window(label))
        # The same native window, existing process and profile must survive.
        evidence = {"before": initial, "after": actual, "activeWorkspace": runtime.ctl("activeworkspace", True), "url": evaluate(session, "location.href"), "browserVersion": cdp.call("Browser.getVersion")["product"], "focusOnActivate": focus_enabled}
        (runtime.base / (label + "-observed.json")).write_text(json.dumps(evidence, indent=2))
        assert actual["address"] == address and actual["pid"] == pid and browser.poll() is None, evidence
        assert actual["workspace"]["name"] == "1", evidence
        expected_workspace = "1" if focus_enabled else str(workspace)
        assert runtime.ctl("activeworkspace", True)["name"] == expected_workspace, evidence
        png = cdp.call("Page.captureScreenshot", {}, session)["data"]
        (runtime.base / (label + ".png")).write_bytes(base64.b64decode(png))
        if label == "cross-workspace":
            subprocess.run(["grim", "-o", "physical", str(runtime.base / "browser-workspace.png")], env=runtime.env, check=True)
        record(label + ": page really loads in the unchanged browser window/workspace", evidence)

    click_link("cross-workspace", 2, True)
    click_link("same-workspace", 1, True)
    runtime.ok("eval hl.config({misc={focus_on_activate=false}})")
    click_link("urgency-only", 2, False)
    runtime.ok("eval hl.config({misc={focus_on_activate=true}})")
    # A real new browser window still maps on the requesting workspace.
    new_page = runtime.base / "new.html"; new_page.write_text("<title>Activation new browser</title><h1>New browser window</h1>")
    focus_workspace(2)
    runtime.start(["/opt/google/chrome/chrome", "--no-sandbox", "--password-store=basic", "--ozone-platform=wayland", "--user-data-dir=" + str(profile), "--new-window", new_page.as_uri()], "new-browser")
    fresh = wait(lambda: window("Activation new browser"))
    assert fresh["workspace"]["name"] == "2" and fresh["address"] != address
    record("real new browser window maps in requesting workspace", fresh)

    # Protocol probes cover pre-map attribution, parent precedence, one-use
    # tokens, domain validation and hide/remap using real Wayland resources.
    protocol_dir = Path(subprocess.check_output(["pkg-config", "--variable=pkgdatadir", "wayland-protocols"], text=True).strip())
    protocols = {"xdg": protocol_dir / "stable/xdg-shell/xdg-shell.xml", "foreign": protocol_dir / "unstable/xdg-foreign/xdg-foreign-unstable-v2.xml", "activation": protocol_dir / "staging/xdg-activation/xdg-activation-v1.xml"}
    for name, xml in protocols.items():
        for mode, ext in (("client-header", "h"), ("private-code", "c")):
            subprocess.run(["wayland-scanner", mode, str(xml), str(runtime.base / (name + "." + ext))], check=True)
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client"], text=True).split()
    subprocess.run(["cc", "-I" + str(runtime.base), str(SOURCE / "routing-client.c"), *(str(runtime.base / (name + ".c")) for name in protocols), "-o", str(runtime.base / "client"), *flags], check=True)
    p = protocol_client("roots")
    focus_workspace(1); command(p, "create token-root"); command(p, "map 0")
    root = wait(lambda: window("token-root"))
    focus_workspace(2)
    action = context(); command(p, "activate " + action["token"] + " 0")
    assert window("token-root")["workspace"] == root["workspace"]
    command(p, "create replay-root"); command(p, "activate " + action["token"] + " 1"); command(p, "map 1")
    assert wait(lambda: window("replay-root"))["workspace"]["name"] == "1"
    record("existing-root activation stays put and token replay is rejected", window("token-root"))
    command(p, "unmap 0"); wait(lambda: not window("token-root"))
    focus_workspace(2); command(p, "activate " + context()["token"] + " 0"); command(p, "map 0")
    assert wait(lambda: window("token-root"))["workspace"]["name"] == "1"
    record("hide/remap keeps established workspace", window("token-root"))
    focus_workspace(2)
    command(p, "create premap-root"); command(p, "activate " + context()["token"] + " 2"); focus_workspace(1); command(p, "map 2")
    assert wait(lambda: window("premap-root"))["workspace"]["name"] == "2"
    record("fresh pre-map root keeps accepted source workspace", window("premap-root"))
    focus_workspace(2); command(p, "create child-root"); command(p, "parent 3 0"); command(p, "activate " + context()["token"] + " 3"); command(p, "map 3")
    assert wait(lambda: window("child-root"))["workspace"]["name"] == "1"
    record("new transient follows parent instead of token workspace", window("child-root"))
    for name in ("A", "B"):
        runtime.ok("seat create " + name + " physical"); runtime.ok("seat workspace " + name + " name:activation-" + name)
    original = window("token-root")
    command(p, "activate " + context("A")["token"] + " 0")
    assert window("token-root")["workspace"] == original["workspace"]
    command(p, "create agent-root"); command(p, "activate " + context("A")["token"] + " 4"); command(p, "map 4")
    agent = wait(lambda: window("agent-root"))
    for source_name in ("B", "main"):
        command(p, "activate " + context(source_name)["token"] + " 4")
        assert window("agent-root")["workspace"] == agent["workspace"]
    command(p, "activate " + context("B")["token"] + " 3")
    assert window("child-root")["workspace"]["name"] == "1"
    record("human/Agent and Agent A/B activations cannot transfer existing roots or children", agent)
    # Fresh valid same-domain token still activates; invalid token does nothing.
    command(p, "activate " + context("A")["token"] + " 4")
    assert runtime.ctl("seat state A", True)["windowAddress"] == agent["address"]
    command(p, "activate nonexistent-token 0")
    assert window("token-root")["workspace"] == original["workspace"]
    record("valid owner activation works while invalid tokens remain rejected", agent)
finally:
    if runtime.compositor and runtime.compositor.poll() is None:
        (runtime.base / "final-clients.json").write_text(json.dumps(runtime.ctl("clients", True), indent=2))
    if cdp: cdp.close()
    runtime.cleanup()
