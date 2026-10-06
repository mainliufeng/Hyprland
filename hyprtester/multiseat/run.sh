#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$source_dir/../.." && pwd)
test_dir=$(mktemp -d /tmp/hyprland-multiseat.XXXXXX)
for protocol in virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1; do
    xml="$repo_dir/hyprtester/protocols/$protocol.xml"
    if [[ ! -f "$xml" ]]; then xml="$repo_dir/protocols/$protocol.xml"; fi
    case "$protocol" in
        virtual-keyboard-*) name=virtual-keyboard ;;
        *) name=virtual-pointer ;;
    esac
    wayland-scanner client-header "$xml" "$test_dir/$name.h"
    wayland-scanner private-code "$xml" "$test_dir/$name.c"
done
cc -I"$test_dir" "$source_dir/input.c" "$test_dir/virtual-pointer.c" "$test_dir/virtual-keyboard.c" -o "$test_dir/input" $(pkg-config --cflags --libs wayland-client xkbcommon)
cc "$source_dir/registry.c" -o "$test_dir/registry" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header "$repo_dir/protocols/input-method-unstable-v2.xml" "$test_dir/input-method.h"
wayland-scanner private-code "$repo_dir/protocols/input-method-unstable-v2.xml" "$test_dir/input-method.c"
cc -I"$test_dir" "$source_dir/ime.c" "$test_dir/input-method.c" -o "$test_dir/ime" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header /usr/share/wayland-protocols/staging/ext-session-lock/ext-session-lock-v1.xml "$test_dir/session-lock.h"
wayland-scanner private-code /usr/share/wayland-protocols/staging/ext-session-lock/ext-session-lock-v1.xml "$test_dir/session-lock.c"
cc -I"$test_dir" "$source_dir/lock.c" "$test_dir/session-lock.c" -o "$test_dir/lock" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header "$repo_dir/protocols/wlr-layer-shell-unstable-v1.xml" "$test_dir/layer-shell.h"
wayland-scanner private-code "$repo_dir/protocols/wlr-layer-shell-unstable-v1.xml" "$test_dir/layer-shell.c"
wayland-scanner client-header "$repo_dir/subprojects/hyprland-protocols/protocols/hyprland-focus-grab-v1.xml" "$test_dir/focus-grab.h"
wayland-scanner private-code "$repo_dir/subprojects/hyprland-protocols/protocols/hyprland-focus-grab-v1.xml" "$test_dir/focus-grab.c"
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$test_dir/xdg-shell.c"
cc -I"$test_dir" "$source_dir/layer.c" "$test_dir/layer-shell.c" "$test_dir/focus-grab.c" "$test_dir/xdg-shell.c" -o "$test_dir/layer" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml "$test_dir/constraints.h"
wayland-scanner private-code /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml "$test_dir/constraints.c"
wayland-scanner client-header /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml "$test_dir/relative-pointer.h"
wayland-scanner private-code /usr/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml "$test_dir/relative-pointer.c"
wayland-scanner client-header /usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml "$test_dir/activation.h"
wayland-scanner private-code /usr/share/wayland-protocols/staging/xdg-activation/xdg-activation-v1.xml "$test_dir/activation.c"
cc -I"$test_dir" "$source_dir/constraints.c" "$test_dir/constraints.c" "$test_dir/relative-pointer.c" "$test_dir/activation.c" -o "$test_dir/constraints" $(pkg-config --cflags --libs gtk+-3.0 wayland-client)
export MULTISEAT_TEST_DIR="$test_dir"
export MULTISEAT_HYPRLAND="${MULTISEAT_HYPRLAND:-$repo_dir/build-multiseat/Hyprland}"
/usr/bin/python3 "$source_dir/probe.py"
