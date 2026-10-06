# Cornice: human and agent seats in one Hyprland process

This fork adds **virtual desktop seats** for concurrent human and agent use.
The human keeps the existing physical input devices and desktop. An agent gets
its own Wayland seat, virtual pointer/keyboard, output, focused window and active
workspace. All applications are managed and rendered by the same Hyprland
process. The implementation never swaps a global “current seat”.

Fork: <https://github.com/mainliufeng/Hyprland>.
Upstream base: `5a78b5e927345860a27e2893bf894f97ee620c48`.

## Run an agent desktop

These commands target an instance of **this fork**. Build and start it in a
separate development session first; the tests below do that without replacing
the running desktop. `hyprctl` must use that instance's
`HYPRLAND_INSTANCE_SIGNATURE` and `XDG_RUNTIME_DIR`.

```sh
hyprctl output create headless agent
hyprctl eval 'hl.monitor({output="agent",mode="1280x800",position="1280x0",scale=1})'
# Keep the primary seat on your human output; replace HUMAN-OUTPUT below.
hyprctl dispatch 'hl.dsp.focus({monitor="HUMAN-OUTPUT"})'
hyprctl seat create agent agent
hyprctl -j seat list
```

Choose output position and resolution to fit your monitor arrangement. The
output must exist, be enabled, be non-mirrored, and not be the primary seat's
currently focused output. Seat creation reserves the entire output. A primary
pointer cannot wander onto a reserved agent output.

`seat list` returns the agent's `display` (for example `seat-agent-5`), cursor in
compositor-global coordinates, workspace and focused window title. The display
is an additional socket in the **same compositor**, with a private registry view:
clients see their own `wl_seat` and output. Ordinary Wayland toolkits therefore
use the correct seat without patches or application title rules.

```sh
agent_display=$(hyprctl -j seat list | jq -r '.[] | select(.name=="agent") | .display')
env -u WAYLAND_SOCKET -u DISPLAY WAYLAND_DISPLAY="$agent_display" \
    dbus-run-session -- your-native-wayland-application
hyprctl seat workspace agent 7
hyprctl seat workspace agent name:research
# Read the agent output through its own connection:
env -u WAYLAND_SOCKET WAYLAND_DISPLAY="$agent_display" grim -o agent agent.png
# End the seat; optionally remove its output afterwards.
hyprctl seat remove agent
hyprctl output remove agent
```

Use a separate application profile where the application requires one (notably
browsers). A reused browser/DBus singleton can otherwise open a window in an
existing human process. `WAYLAND_SOCKET` must be unset because an inherited file
descriptor overrides `WAYLAND_DISPLAY`.

The input driver must bind the advertised seat and pass it to
`zwp_virtual_keyboard_manager_v1.create_virtual_keyboard` and
`zwlr_virtual_pointer_manager_v1.create_virtual_pointer_with_output`.
A Linux `uinput`/`ydotool` device still belongs to the human's physical input path.
The real protocol driver in `hyprtester/multiseat/input.c` provides a working
reference. Cornice's AI integration is a separate consumer of this interface.

## Independent state and lifecycle

| State | Implementation |
| --- | --- |
| Mouse and cursor | Per-seat pointer manager; software cursor on the reserved output; cursor surfaces and shape requests retain their seat |
| Keyboard | Per-seat keymap, pressed keys, modifiers, focus, handler stack and client repeat data |
| Navigation | Reserved output and its active workspace; `seat workspace` changes only that output |
| Windows and popups | Application connection determines output ownership; focus, activation tokens, popup grabs, rendering and client move/resize use the seat controller |
| Input grabs and constraints | Focus grabs, relative motion, locked/confined pointers and warp use the requesting seat |
| Clipboard, primary selection, drag | Per-seat devices, sources, offers and drag state |
| Text input | Per-seat text input/IME relay and keyboard grab routing |
| Lock and idle | Agent input stops during session lock; cannot type into the human unlock surface; does not trigger human input wake/idle policy |

Workspace IDs/names remain compositor-wide and cannot be claimed from another
output. This is an output reservation model: a human and agent do not concurrently
navigate the same output or manipulate the same application surface. The agent's
workspace stays rendered on its virtual output while the human uses another
output. Arbitrary invisible workspaces on the human's output are not a second
independent desktop.

Removing a seat closes its listening socket, removes its seat global, releases
buttons/focus/grabs, disables input and hides its windows/layers. Existing client
resources remain valid but inert until their connections close. The compositor
does not kill their processes. A recreated name gets a new socket and controller;
old clients are never reassigned to it. Disconnected retired controllers are
collected on later seat create/remove commands. Output disconnection also retires
its seat.

## Scope and limits

- Implemented for native Wayland applications with virtual agent keyboard and
  pointer input. Physical input remains on the primary seat. Physical device
  reassignment, secondary tablet/touch devices and secondary XWayland are outside
  this implementation.
- The existing compositor keybind/dispatcher system continues to target the
  primary seat. Agent navigation uses the explicit `seat workspace` API; agent
  keys go to its application. Global privileged IPC is not implicitly redirected.
- This is **interaction isolation**, not a security sandbox or a second Unix
  login. The same UID, compositor, configuration, filesystem and privileged
  protocols are shared. Use separate OS/application isolation for untrusted code.
  Existing clients that bound outputs before reservation keep those resources.
- Applications can share document state even with separate windows. Use separate
  profiles/processes and an ownership policy for shared documents.
- Verification uses real native GTK clients on nested/headless outputs. Hardware
  DRM output combinations and every application's toolkit behavior have not been
  certified. The running human session has not been replaced or restarted.

## Build and verify

On the tested Arch environment: Clang 22, Lua 5.5, aquamarine 0.15.1,
hyprutils 0.14.2, system Python 3.14 and the upstream build dependencies.

```sh
git submodule update --init --recursive
cmake -S . -B build-multiseat -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS_DEBUG='-O0 -g0' \
  -DCMAKE_C_FLAGS_DEBUG='-O0 -g0' -DUSE_TRACY=OFF -DNO_HYPRPM=ON \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build-multiseat -j4
./build-multiseat/hyprland_gtests
make -C hyprtester/plugin CXX=clang++ LUA_INCLUDES=/usr/include/lua5.5
MULTISEAT_REGRESSION=1 ./hyprtester/multiseat/run.sh
```

The harness needs Mutter (headless parent), dbus-daemon, system Python with
PyGObject/GTK3, GTK3 development headers, GCC/pkg-config, wayland-scanner,
wayland-protocols, xkbcommon, grim, wl-clipboard and hyprctl. A running host
Hyprland is queried **read-only** for physical device names. The private fork
starts with those devices disabled, its own runtime directory and DBus, and no
DRM device. Test input clients connect only to the private compositor's sockets.

The harness records `results.json`, client/compositor logs and actual output
screenshots under the printed `/tmp/hyprland-multiseat.*` directory. Its assertions
cover concurrent typing/clicking, modifier separation, clipboard and primary
selection, real GTK popup and payload drag, two Chinese IME commits and independent IME keyboard grabs, real session
lock/unlock, release-before-children lifetime, pointer confinement/relative
motion, activation tokens, client window move/resize, layer-shell/focus grab, held-input seat removal,
same-name recreation and collection after disconnect. The optional regression
run exercises five upstream single-seat integration tests in another private
fork instance: keyboardModifiersMergedOnFocus, pointerWarp, xdgInteractive,
popupOpacityInheritsParentFade and xdgActivationSerial.

See [recorded verification](verification/cornice-multiseat.md) for the observed
results and visually inspected output captures. The two small build compatibility
fixes (Clang coverage linking and workspace-swipe construction) allow the upstream
Debug/unit-test configuration to build on this host.

Upstream contributions must follow the repository's [AI usage policy](https://github.com/hyprwm/.github/blob/main/policies/AI_USAGE.md)
and [issue guidelines](https://wiki.hypr.land/contributing-and-debugging/issue-guidelines/).
This delivery is in the user's fork; no upstream PR, issue or discussion was opened.
