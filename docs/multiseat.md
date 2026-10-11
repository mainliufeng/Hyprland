# Experimental multi-seat controller interface

This branch adds independent input seats and workspace views to one compositor.
Windows and workspace objects are shared; input focus, pointer, key state, IME,
selection, grabs and current workspace belong to a seat. A secondary seat does
not require a physical output of its own. Applications that bind only one seat
must be launched using the selected seat socket. X11 secondary-seat support is
not guaranteed. These interfaces are experimental and are not upstream APIs.

## Identity and control

Query `hyprctl -j seat capabilities` before using extensions. IPC must target the
same compositor instance for the lifetime of a controller. `seat state NAME`
returns seatId, generation, display, output, workspace, cursor and control state;
`main` refers to the primary desktop. `seat list` exposes secondary seat states.

Create with `seat create NAME OUTPUT`, then select with `seat workspace NAME WS`.
The returned Wayland display targets application startup and virtual devices.
An output and workspace can be shared by multiple seats. A socket determines
default input and startup context, not application ownership or a Linux-user
security boundary. Do not assume late-added seats work in every toolkit.

Use `seat control NAME SEAT_ID GENERATION pause|resume` for managed input.
Revocation releases held inputs and increments the generation; old connections
and devices cannot reacquire input. Unlock does not resume revoked control.
`seat act NAME SEAT_ID GENERATION workspace|focus ARGUMENT` checks live identity.
`seat snapshot NAME SEAT_ID current|EXISTING_WS png|argb ABS_PATH` exports an
atomic frame without changing focus or workspace. Frame IDs and timestamps
identify the capture; locks revoke protected exports.
`seat export-grant NAME SEAT_ID GENERATION GRANT` and
`seat authorized-snapshot NAME SEAT_ID current png|argb ABS_PATH GENERATION GRANT`
provide generation-bound authorized capture. The initial `agent-snapshot`
spelling remains a compatibility alias with exactly the same authorization
checks. Authorization is scoped to the live seat and its current workspace;
it is not an application-specific API or an OS user boundary.

## Native views and scoped configuration

`seat present NAME SEAT_ID OUTPUT current|EXISTING_WS OWNER` creates a native
readonly view. Its browse workspace is independent of the seat. Ordinary
application input and mutating bindings are blocked. `seat present-control OWNER yes` routes
physical input through that seat's native managers and revokes automation.
`physicalControl` reports whether this input route is active. The initial
`humanControl` response field remains an alias for existing controllers; it does
not classify a seat as human or automated.
`seat unpresent OWNER` restores the physical desktop. No image forwarding is used.

`seat configure JSON` installs managed bindings and overlay rules in the native
registry. Required fields are owner (32–128 characters), seatName and seatId;
optional callback, bindings and overlays declare controller behavior. Configuration
has a five-second lease renewed by `seat configuration-renew OWNER`; removing it
with `seat configuration-remove OWNER` restores inherited bindings. Reload requires
explicit restoration. Limits: 64 controllers, 128 bindings/overlays, 64 KiB JSON.

A binding specifies keys, action, argument, viewOnly and overrideInherited.
Actions are workspace, move, dismiss and notify; readonly move is rejected.
Inherited conflicts require explicit override and are reported in the response.
Concurrent controllers cannot claim the same seat/mode/key combination. Overlays
specify namespace name, actual client pid, keyboard and localInView; they have no
hardcoded application names. A management overlay may receive its own input in
readonly mode without sending input to observed applications.

`seat ensure-workspace NAME SEAT_ID WS` explicitly reserves an empty workspace,
up to 128 per seat. The compositor does not interpret product workspace names.
Configured view-navigation actions may explicitly reserve their chosen workspace.

## Actions and asynchronous dispatch

notify sends a unicast controller-action JSON request to callback, an absolute
socket under XDG_RUNTIME_DIR. Its params include actionId, owner, seat/seatId,
generation, viewOwner, mode and action. Validate using
`seat validate-context ACTION_ID OWNER` before handling external work.

Native exec exposes HYPRLAND_ACTION_ID and HYPRLAND_SEAT_* for the triggering seat.
`seat context-dispatch ACTION_ID DISPATCH` retains the anonymous native-exec form.
Controller notifications use `seat context-dispatch ACTION_ID OWNER DISPATCH`,
advertised by `owned-seat-action-context-v1`; a missing or different owner is
rejected. Both forms perform asynchronous actions only while the five-second
context remains valid. Readonly contexts cannot dispatch. Identity, generation,
view mode/workspace and lock epoch changes reject pending work; stale contexts
never target primary.
These are trusted same-UID management APIs, not credentials for an OS sandbox.

## Explicit lock scopes

`seat management-display` returns `{display}` for an explicit management Wayland
socket, advertised by the `management-display-v1` capability. Its connections
retain primary default input context and expose private `wl_output` objects plus
scoped and ordinary lock managers, so a lock controller can render every protected
output. Ordinary application sockets continue hiding private outputs; secondary
seat sockets continue hiding the scope manager. This is a trusted same-UID
management role, not a Linux-user security boundary.

The experimental `hyprland-lock-scope-v1` protocol configures a lock before activate.
It protects all seats and outputs by default. Explicit allow_seat requires matching
name/identity/generation and prior scope-policy authorization; exclude_output is
limited to registered private headless outputs. New or unknown outputs stay locked.
Scope-policy authorization alone never exempts a seat or output. secure follows
protected frame presentation or confirmation that the protected output is off.

Owner or guardian loss escalates to an orphaned full lock and revokes exemptions.
Standard ext-session-lock-v1 remains the complete-session lock and takes priority.
The shell owns authentication UI, scope selection and sleep coordination; it must
wait for complete-lock secure before permitting sleep. Recovery never grants old
input permissions automatically.

## Regression tests

The native integration suite has no desktop-shell dependency. All graphical
entry points require a built compositor argument and enter the same sandbox;
none reads a host compositor socket, configuration or physical input device.
An executable under `/tmp` is mounted explicitly at
`/opt/hyprland-test/Hyprland`, so hiding the host `/tmp` cannot select an older
installed build accidentally.

Enable the suite explicitly alongside the existing upstream tests:

```sh
cmake -S . -B /tmp/hyprland-test-build \
  -DCMAKE_BUILD_TYPE=Debug -DWITH_MULTISEAT_TESTS=ON
cmake --build /tmp/hyprland-test-build -j2
HYPRLAND_TEST_RENDER_NODE=/dev/dri/renderD128 \
  ctest --test-dir /tmp/hyprland-test-build --output-on-failure -L multiseat
ctest --test-dir /tmp/hyprland-test-build --output-on-failure -LE multiseat
```

`WITH_MULTISEAT_TESTS` defaults to off and requires `BUILD_TESTING` or
`WITH_TESTS`. Missing dependencies or a failed regression are errors, with no
silent skip. CMake checks Bash, Bubblewrap, systemd-run, flock, coreutils timeout
and nice, D-Bus, Mutter, a C compiler, pkg-config, wayland-scanner, wl-clipboard,
kitty/kitten, xeyes, jq, hyprctl, XWayland, procps (pgrep/ps), a POSIX shell and
coreutils (sleep/cat/touch/realpath/env/mkdir/mktemp),
Wayland client/protocol development files, xkbcommon, GTK 3, Python GObject
bindings and the GTK/GdkPixbuf typelibs. A running systemd user manager and
permitted unprivileged user namespaces are required at execution. The complete
upstream integration runner additionally needs its existing runtime applications
(`kitty`/`kitten`, `xeyes`, `jq`, `hyprctl`, `Xwayland`, `pgrep`, `ps`,
`sleep`, `cat` and `touch`) and all matching built test clients. An XWayland-enabled
build is required for its full test set. `HYPRLAND_TEST_TOOLS_DIR` may name an
additional directory of real validation tools; CMake searches it and the sandbox
mounts it read-only, including when the directory is under `/tmp`. This supplies
dependencies without replacing a missing application with a test double.
CTest supplies the matching built `hyprctl` target. Direct invocations can set
`HYPRLAND_TEST_HYPRCTL`; its executable is explicitly mounted at
`/opt/hyprland-test/hyprctl` and placed first in PATH, so a build under `/tmp`
remains usable and does not select an older installed CLI accidentally. Select an interpreter
with `Python3_EXECUTABLE` at configure time or `HYPRLAND_TEST_PYTHON` for a direct
invocation when the default Python lacks GObject bindings.

The five CTest entries are:

- `integration.controller`: real GTK input, generic binding/configuration leases,
  asynchronous action contexts, readonly native observation, physical takeover,
  authorized exports, revocation and scoped-lock defaults.
- `integration.multiseat`: concurrent input on four seats, shared workspaces,
  real clipboard/primary selection, IME grabs, drag/drop, popups, cursor surfaces,
  activation, pointer constraints, lock gates and retirement. It also runs five
  relevant upstream single-seat regressions.
- `integration.lifecycle`: empty-keymap device JSON, first keyboard before and
  after exclusive-layer mapping, first key during full lock, the ordinary
  `allow_session_lock_restore` true/false and live/orphan owner matrix, scoped
  to full upgrade with real frames on every protected output, and a compositor
  crash/restart leaving the previous seat socket in place. A finished previous
  owner attempts stale unlock/destroy after replacement; independent scoped
  owner and guardian loss must revoke exemptions and require full-lock recovery.
  Real registry clients verify that the explicit management socket sees private
  outputs and lock protocols while ordinary application filtering stays intact.
  Existing idle clients on preferred and default shared sockets cannot regain
  input by first creating devices after pause/resume. On the primary seat, an
  authorized PID cannot regain input by creating or recreating devices on its old client;
  reconnecting the same process creates a new client that can type normally.
- `integration.dispatcher`: native relative and monitor workspace selectors,
  independent MRU history and both back-and-forth modes, empty-workspace monitor
  focus fallback, primary pointer constraint retention during secondary focus,
  and two seats independently inhibiting shortcuts on one shared surface.
- `integration.single-seat`: the complete existing upstream `hyprtester` suite,
  using the explicitly selected compositor, matching plugin and built clients.

CTest marks these serial and gives them a shared resource lock and a 20-minute
timeout. Direct entry points also hold one per-UID flock and require a systemd
scope limiting CPU to two cores, memory to 3 GiB, swap to zero, tasks to 512 and
runtime to 20 minutes. A 19-minute inner timeout reserves time for cleanup.
Each run has private HOME, runtime, D-Bus, PID and network namespaces. Physical
input devices, DRM modesetting devices and host display/system-bus sockets are
hidden. Test children cannot write the host filesystem.

Software rendering is the default. `HYPRLAND_TEST_RENDER_NODE` explicitly opts
into sharing one existing `/dev/dri/renderD*` character device; card and input
devices remain hidden. Some Aquamarine/Mutter versions need this to obtain
DMA-BUF support. A software-renderer failure is reported as a failure rather
than retried against a host compositor. Artifacts and logs are retained under
`/tmp/hyprland-multiseat.*`.

Direct equivalents are:

```sh
hyprtester/multiseat/generic-controller.sh /absolute/path/to/Hyprland
hyprtester/multiseat/sandbox.sh lifecycle /absolute/path/to/Hyprland
hyprtester/multiseat/sandbox.sh dispatcher /absolute/path/to/Hyprland
hyprtester/multiseat/run.sh /absolute/path/to/Hyprland \
  /absolute/path/to/hyprtester /absolute/path/to/hyprtestplugin.so \
  /absolute/path/to/built/test/client/directory
hyprtester/multiseat/sandbox.sh single-seat /absolute/path/to/Hyprland \
  /absolute/path/to/hyprtester /absolute/path/to/hyprtestplugin.so \
  /absolute/path/to/built/test/client/directory
```

Only the direct `single-seat` entry point accepts optional native test names
after its five fixed arguments. For example, append `xdgInteractive pointerWarp`
to compare those upstream cases on two builds. Arguments are forwarded
as individual values to the upstream runner, which rejects unknown test names.
Without selectors it runs the complete suite, including the default CTest entry.
Other entry points continue requiring their exact argument counts.

`seat-controller-check` and `multiseat-check` are convenience build targets when
this option is enabled. The existing GTest, full upstream integration, release
build with warnings as errors, code-format check and Nix VM checks remain part
of upstream validation. The Nix test must fail when either existing test runner
exits unsuccessfully; recording an exit code alone is insufficient validation.
