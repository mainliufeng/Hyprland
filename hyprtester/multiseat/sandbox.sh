#!/usr/bin/env bash
# All graphical checks use one bounded private compositor; never a host socket.
set -euo pipefail
ulimit -c 0
[[ $# -ge 2 ]] || { echo "Usage: sandbox.sh SUITE /absolute/path/Hyprland [HYPRTESTER PLUGIN CLIENT_DIRECTORY [TEST_NAMES... (single-seat only)]]" >&2; exit 2; }
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$source_dir/../.." && pwd)
suite=$1
tools_mount=()
if [[ -n ${HYPRLAND_TEST_TOOLS_DIR:-} ]]; then
    tools_dir=$(realpath "$HYPRLAND_TEST_TOOLS_DIR")
    [[ -d $tools_dir ]] || { echo "Additional test tools directory unavailable: $tools_dir" >&2; exit 2; }
    export PATH="$tools_dir:$PATH"
    # Explicit additional real tools remain visible when the host /tmp is hidden.
    tools_mount=(--ro-bind "$tools_dir" "$tools_dir")
fi
binary=$(realpath "$2")
case "$suite" in controller|multiseat|lifecycle|dispatcher|single-seat|routing|idle-query) ;; *) echo "Unknown suite: $suite" >&2; exit 2 ;; esac
[[ -f $binary && -x $binary ]] || { echo "Built compositor required: $binary" >&2; exit 2; }
for tool in bash sh bwrap systemd-run flock timeout nice dbus-daemon mutter python3 cc pkg-config wayland-scanner realpath env mkdir mktemp; do
    command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 2; }
done
python=${HYPRLAND_TEST_PYTHON:-$(command -v python3)}
[[ -x $python ]] || { echo "Test Python interpreter unavailable: $python" >&2; exit 2; }
pkg-config --exists wayland-client xkbcommon wayland-protocols gtk+-3.0
"$python" -c 'import gi; gi.require_version("Gtk", "3.0"); gi.require_version("GdkPixbuf", "2.0"); from gi.repository import Gtk, GdkPixbuf'
if [[ $suite == multiseat ]]; then
    for tool in wl-copy wl-paste; do command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 2; }; done
fi
hyprctl_mount=()
if [[ $suite == multiseat || $suite == single-seat ]]; then
    # These are real commands invoked by the unchanged upstream suite, including
    # its kitty remote control, device JSON parity and XWayland cases.
    for tool in kitty kitten xeyes jq Xwayland pgrep ps sleep cat touch; do
        command -v "$tool" >/dev/null || { echo "Missing upstream test dependency: $tool" >&2; exit 2; }
    done
    hyprctl_path=${HYPRLAND_TEST_HYPRCTL:-}
    if [[ -z $hyprctl_path ]]; then
        hyprctl_path=$(command -v hyprctl || true)
    fi
    [[ -f $hyprctl_path && -x $hyprctl_path ]] || { echo 'The upstream suite requires a real hyprctl; set HYPRLAND_TEST_HYPRCTL to the matching build' >&2; exit 2; }
    hyprctl_path=$(realpath "$hyprctl_path")
    hyprctl_mount=(--ro-bind "$hyprctl_path" /opt/hyprland-test/hyprctl)
fi
if [[ $suite == routing || $suite == idle-query ]]; then
    hyprctl_path=${HYPRLAND_TEST_HYPRCTL:-$(dirname "$binary")/hyprctl/hyprctl}
    [[ -f $hyprctl_path && -x $hyprctl_path ]] || { echo 'Routing requires a matching built hyprctl' >&2; exit 2; }
    hyprctl_path=$(realpath "$hyprctl_path")
    hyprctl_mount=(--ro-bind "$hyprctl_path" /opt/hyprland-test/hyprctl)
fi
mounts=(--ro-bind "$repo_dir" "$repo_dir" --ro-bind "$binary" /opt/hyprland-test/Hyprland)
selectors=()
if [[ $suite == multiseat || $suite == single-seat ]]; then
    [[ $# -ge 5 && ( $suite == single-seat || $# == 5 ) ]] || { echo 'Hyprtester, matching plugin and built client directory are required; only single-seat accepts optional native test names' >&2; exit 2; }
    if [[ $suite == single-seat ]]; then selectors=("${@:6}"); fi
    tester=$(realpath "$3"); plugin=$(realpath "$4"); clients=$(realpath "$5")
    [[ -x $tester && -f $plugin && -d $clients ]] || { echo 'Missing upstream integration artifacts' >&2; exit 2; }
    mounts+=(--ro-bind "$tester" /opt/hyprland-test/hyprtester --ro-bind "$plugin" /opt/hyprland-test/hyprtestplugin.so --ro-bind "$clients" "$clients")
else
    [[ $# == 2 ]] || { echo 'Unexpected test arguments' >&2; exit 2; }
fi
# CTest's resource lock also serializes, but direct invocations need this lock.
# Scope creation is required: never silently run a GPU test without limits.
if [[ ${HYPRLAND_TEST_RESOURCE_SCOPE:-} != 1 ]]; then
    exec flock "/tmp/hyprland-multiseat-test-${UID}.lock" \
        systemd-run --user --scope --quiet --collect --expand-environment=no \
        --unit="hyprland-multiseat-test-${UID}-$$" --description='Isolated Hyprland integration tests' \
        --property=CPUQuota=200% --property=MemoryMax=3G --property=MemorySwapMax=0 \
        --property=TasksMax=512 --property=RuntimeMaxSec=20min \
        env HYPRLAND_TEST_RESOURCE_SCOPE=1 "$0" "$@"
fi
render=(--setenv LIBGL_ALWAYS_SOFTWARE 1 --setenv GALLIUM_DRIVER llvmpipe)
if [[ -n ${HYPRLAND_TEST_RENDER_NODE:-} ]]; then
    [[ $HYPRLAND_TEST_RENDER_NODE =~ ^/dev/dri/renderD[0-9]+$ && -c $HYPRLAND_TEST_RENDER_NODE ]] || { echo 'Only an existing DRM render node can be exposed' >&2; exit 2; }
    render=(--dir /dev/dri --dev-bind "$HYPRLAND_TEST_RENDER_NODE" "$HYPRLAND_TEST_RENDER_NODE" --unsetenv LIBGL_ALWAYS_SOFTWARE --unsetenv GALLIUM_DRIVER)
fi
artifacts=$(mktemp -d /tmp/hyprland-multiseat.XXXXXX)
mkdir -m700 "$artifacts/home"
echo "Isolated artifacts: $artifacts"
exec nice -n 15 timeout --signal=TERM --kill-after=10s 19m \
    bwrap --unshare-all --die-with-parent --new-session \
    --ro-bind / / --dev /dev --proc /proc --tmpfs /run --tmpfs /tmp --tmpfs /opt \
    --dir /opt/hyprland-test --bind "$artifacts" /tmp/t "${mounts[@]}" "${hyprctl_mount[@]}" "${tools_mount[@]}" "${render[@]}" \
    --setenv PATH "/opt/hyprland-test:$PATH" --setenv HYPRLAND_TEST_HYPRCTL /opt/hyprland-test/hyprctl \
    --setenv HOME /tmp/t/home --setenv TMPDIR /tmp/t --setenv XDG_RUNTIME_DIR /tmp/t \
    --setenv XDG_CONFIG_HOME /tmp/t/home/.config --setenv XDG_DATA_HOME /tmp/t/home/.local/share \
    --setenv XDG_CACHE_HOME /tmp/t/home/.cache --setenv XDG_STATE_HOME /tmp/t/home/.local/state \
    --setenv MULTISEAT_TEST_DIR /tmp/t --setenv MULTISEAT_HYPRLAND /opt/hyprland-test/Hyprland \
    --setenv MULTISEAT_HYPRTESTER /opt/hyprland-test/hyprtester --setenv MULTISEAT_PLUGIN /opt/hyprland-test/hyprtestplugin.so \
    --setenv HYPRLAND_TEST_SANDBOX 1 --setenv HYPRLAND_TEST_PYTHON "$python" --setenv PYTHONDONTWRITEBYTECODE 1 \
    --setenv LP_NUM_THREADS 2 --setenv OMP_NUM_THREADS 2 --setenv HYPRLAND_HEADLESS_ONLY 1 \
    --setenv AQ_DRM_DEVICES /dev/null --setenv LIBSEAT_BACKEND noop \
    --setenv DBUS_SYSTEM_BUS_ADDRESS unix:path=/run/no-system-bus \
    --unsetenv HYPRLAND_INSTANCE_SIGNATURE --unsetenv WAYLAND_DISPLAY --unsetenv WAYLAND_SOCKET \
    --unsetenv DISPLAY --unsetenv DBUS_SESSION_BUS_ADDRESS --unsetenv HYPRLAND_ACTION_ID \
    --unsetenv HYPRLAND_SEAT_NAME --unsetenv HYPRLAND_SEAT_ID --unsetenv HYPRLAND_SEAT_GENERATION \
    -- bash "$source_dir/suite.sh" "$suite" "${selectors[@]}"
