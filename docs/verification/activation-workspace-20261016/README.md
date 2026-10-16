# Existing-window activation must preserve workspace

Validated on 2026-10-16 against our own fork, starting from published/running
`b5ae528d2df0f02319500f743c37984ce07e6fde`. This is a workspace-routing bug;
it does not require a request to come from a different Cornice desktop domain.

## Cause and change

`src/protocols/XDGActivation.cpp` previously passed every valid activation token's
workspace to `CWindow::routeTo()`, including an already presented toplevel.
`routeTo()` moves mapped windows through `moveWindowToWorkspace()`. Thus a valid
source snapshot was incorrectly treated as authority to move the destination.
The token checks themselves were not the problem.

`CWindow::activateFromToken()` now separates initial attribution from activation:

- An unpresented root may receive the token's initial workspace/domain.
- A presented root keeps its workspace. An ordinary same-domain activation still
  passes through the existing urgency/focus-on-activate policy.
- Presentation history survives unmap. `onUnmap()` clears the current workspace,
  so a hidden window uses the existing `workspaceAddress()` history when handling
  activation before remap. This does not resurrect a deleted workspace.
- A child follows its explicit parent. Tokens from a different owning domain
  cannot activate or transfer an established root/child, including human/Agent
  and Agent A/B boundaries. A valid owner token still works.
- Source serial, generation, lock epoch, lifetime and one-use validation remain
  in the protocol layer. No application-name, title, output or workspace-number
  condition was added to product code.

The historical `822025e` was inspected but not cherry-picked: it only guarded an
Agent request against a presented primary-owned target and did not fix ordinary
same-domain cross-workspace movement. Cornice `6b1653f` contains feedback/tray
changes, not this compositor fix. Both older investigation workspaces remain.

## Actual link chain and evidence boundary

The installed Grok Bot ASAR's appWindow `openExternal` method delegates to
`e.shell.openExternalUrl(url)`. Its system-handler path calls Electron
`shell.openExternal(url)`. The installed native executable was not instrumented
or replaced, and no real user's Grok or browser window/profile was operated.
The exact installed Electron native token implementation is not claimed verified.

Electron's [Linux implementation](https://github.com/electron/electron/blob/main/shell/common/platform_util_linux.cc)
uses the system opener and creates a Wayland activation token. Chromium's
[singleton interface](https://chromium.googlesource.com/chromium/chromium/+/refs/heads/main/chrome/browser/process_singleton.h)
and [token injector](https://chromium.googlesource.com/chromium/src/base/+/fc3e5b80819614ec2655c9d2049b64b817c1df1c/nix/scoped_xdg_activation_token_injector.h)
are reference explanations, not assertions about Grok's exact bundled revision.

The executed proof uses installed Electron 38 with a private button that calls
real `shell.openExternal()`, the installed `xdg-open`, a private default-browser
desktop entry, and installed Chrome 154.0.8037.57. A real virtual-keyboard Enter
activates the button. Chrome's second invocation forwards to the existing
process/profile, creates the real HTTP tab, and sends activation to the nested
compositor. A loopback HTTP server and CDP independently verify document title
and URL. PID, native window address and workspace are checked separately.
This reproduces the reported mechanism without claiming an end-to-end click in
the authenticated Grok UI.

`baseline.json`: the same native Chrome window and PID load the new HTTP page,
but move **workspace 1 → 2** on the unchanged published compositor.
`checks.json`: the repaired compositor loads the page in the same native window
and PID and keeps **workspace 1 → 1**. The baseline exits on that exact assertion;
the repaired test exits 0 with all 10 grouped checks passing.

Coverage includes cross/same workspace, singleton reuse, real new browser window,
focus enabled/disabled, token replay/invalid token, fresh pre-map root, hide/remap,
parented child, and human/Agent plus A/B boundaries. `browser.png` is an inspected
capture of the actual isolated browser UI; test-only startup warning overlays
are expected. No mocks or fake browser/opener are used.

The current real session has `misc:focus_on_activate=false`: same-domain
activation retains the source workspace and uses urgency only. With that existing
option true, focus visits the browser's existing workspace; the browser stays
there. Both settings pass. The fix adds no new focus preference.

## Related regression and reproduction

`parent-routing.json`: all 14 grouped checks in the existing routing suite pass,
including real GTK3 cross-connection parent attribution and actual Agent typing
without changing human focus. Obsolete activation-as-domain-transfer assertions
were replaced with ownership-preservation and valid-owner activation assertions.
The GTK test now waits for the actual entry focus event before the first key;
this removes a test readiness race rather than changing product input logic.

Build: Release, clang++, existing dependency revisions, two-CPU/3-GiB systemd
scope. Both suites run through the existing bwrap sandbox: private runtime,
HOME/profile, bus, PID/network namespaces, no host input devices, render node
only. The sandbox's `clients` requests target only its private compositor socket.
No host `clients -a`, window operations, compositor restart or logout occurred.

```sh
HYPRLAND_TEST_RENDER_NODE=/dev/dri/renderD128 \
  hyprtester/multiseat/sandbox.sh activation "$PWD/build-release/Hyprland"
HYPRLAND_TEST_RENDER_NODE=/dev/dri/renderD128 \
  hyprtester/multiseat/sandbox.sh routing "$PWD/build-release/Hyprland"
```

`activation` additionally requires electron38, installed Chrome at its packaged
Arch path, and grim. The test always creates its own default-browser association
and profiles. Basic password storage applies only to those empty test profiles,
so initialization never waits for a desktop keyring. The network namespace has
no external connectivity. The production opener/browser configuration is untouched.

## Delivery boundary

Only our own fork's main is a delivery target; no upstream PR, issue or push.
The fixed compositor must be staged as a new immutable login release, retaining
the current release, old Shell/runtime override and rofi fallback. A future-login
selection is reversible and does not update a running compositor. The live
session remains the old compositor until the owner independently logs in again;
source tests and a prepared release alone do not prove live adoption.
