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

## Native views and scoped configuration

`seat present NAME SEAT_ID OUTPUT current|EXISTING_WS OWNER` creates a native
readonly view. Its browse workspace is independent of the seat. Ordinary
application input and mutating bindings are blocked. `seat present-control OWNER yes` routes
physical input through that seat's native managers and revokes automation.
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
`seat context-dispatch ACTION_ID DISPATCH` performs asynchronous actions only while
the five-second context remains valid. Identity, generation, view mode/workspace
and lock epoch changes reject pending work; stale contexts never target primary.
These are trusted same-UID management APIs, not credentials for an OS sandbox.

## Explicit lock scopes

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

## Regression

`hyprtester/multiseat/generic-controller.sh /absolute/path/to/Hyprland` runs a real,
standalone controller in a device/PID/network/DBus/HOME sandbox, using only a render
node and nested Mutter. It tests bindings, conflicts, unicast action contexts,
independent browse and background actions, revocation, reload/expiry and a lock
scope with no implicit exemptions. `seat-controller-check` is the CMake target.
Existing multiseat fixtures cover applications, input, IME, clipboard and lifecycle.
