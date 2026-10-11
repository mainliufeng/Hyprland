#!/usr/bin/env bash
# Build protocol clients only in the writable private artifact directory.
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$source_dir/../.." && pwd)
protocol_dir=$(pkg-config --variable=pkgdatadir wayland-protocols)
: "${MULTISEAT_TEST_DIR:?Private artifact directory required}"
for protocol in virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1; do
    xml="$repo_dir/hyprtester/protocols/$protocol.xml"
    if [[ ! -f "$xml" ]]; then xml="$repo_dir/protocols/$protocol.xml"; fi
    case "$protocol" in
        virtual-keyboard-*) name=virtual-keyboard ;;
        *) name=virtual-pointer ;;
    esac
    wayland-scanner client-header "$xml" "$MULTISEAT_TEST_DIR/$name.h"
    wayland-scanner private-code "$xml" "$MULTISEAT_TEST_DIR/$name.c"
done
cc -I"$MULTISEAT_TEST_DIR" "$source_dir/input.c" "$MULTISEAT_TEST_DIR/virtual-pointer.c" "$MULTISEAT_TEST_DIR/virtual-keyboard.c" -o "$MULTISEAT_TEST_DIR/input" $(pkg-config --cflags --libs wayland-client xkbcommon)
cc "$source_dir/registry.c" -o "$MULTISEAT_TEST_DIR/registry" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header "$repo_dir/protocols/input-method-unstable-v2.xml" "$MULTISEAT_TEST_DIR/input-method.h"
wayland-scanner private-code "$repo_dir/protocols/input-method-unstable-v2.xml" "$MULTISEAT_TEST_DIR/input-method.c"
cc -I"$MULTISEAT_TEST_DIR" "$source_dir/ime.c" "$MULTISEAT_TEST_DIR/input-method.c" -o "$MULTISEAT_TEST_DIR/ime" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header "$protocol_dir/staging/ext-session-lock/ext-session-lock-v1.xml" "$MULTISEAT_TEST_DIR/session-lock.h"
wayland-scanner private-code "$protocol_dir/staging/ext-session-lock/ext-session-lock-v1.xml" "$MULTISEAT_TEST_DIR/session-lock.c"
cc -I"$MULTISEAT_TEST_DIR" "$source_dir/lock.c" "$MULTISEAT_TEST_DIR/session-lock.c" -o "$MULTISEAT_TEST_DIR/lock" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header "$repo_dir/protocols/wlr-layer-shell-unstable-v1.xml" "$MULTISEAT_TEST_DIR/layer-shell.h"
wayland-scanner private-code "$repo_dir/protocols/wlr-layer-shell-unstable-v1.xml" "$MULTISEAT_TEST_DIR/layer-shell.c"
wayland-scanner client-header "$repo_dir/subprojects/hyprland-protocols/protocols/hyprland-focus-grab-v1.xml" "$MULTISEAT_TEST_DIR/focus-grab.h"
wayland-scanner private-code "$repo_dir/subprojects/hyprland-protocols/protocols/hyprland-focus-grab-v1.xml" "$MULTISEAT_TEST_DIR/focus-grab.c"
wayland-scanner private-code "$protocol_dir/stable/xdg-shell/xdg-shell.xml" "$MULTISEAT_TEST_DIR/xdg-shell.c"
cc -I"$MULTISEAT_TEST_DIR" "$source_dir/layer.c" "$MULTISEAT_TEST_DIR/layer-shell.c" "$MULTISEAT_TEST_DIR/focus-grab.c" "$MULTISEAT_TEST_DIR/xdg-shell.c" -o "$MULTISEAT_TEST_DIR/layer" $(pkg-config --cflags --libs wayland-client)
wayland-scanner client-header "$protocol_dir/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml" "$MULTISEAT_TEST_DIR/constraints.h"
wayland-scanner private-code "$protocol_dir/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml" "$MULTISEAT_TEST_DIR/constraints.c"
wayland-scanner client-header "$protocol_dir/unstable/relative-pointer/relative-pointer-unstable-v1.xml" "$MULTISEAT_TEST_DIR/relative-pointer.h"
wayland-scanner private-code "$protocol_dir/unstable/relative-pointer/relative-pointer-unstable-v1.xml" "$MULTISEAT_TEST_DIR/relative-pointer.c"
wayland-scanner client-header "$protocol_dir/staging/xdg-activation/xdg-activation-v1.xml" "$MULTISEAT_TEST_DIR/activation.h"
wayland-scanner private-code "$protocol_dir/staging/xdg-activation/xdg-activation-v1.xml" "$MULTISEAT_TEST_DIR/activation.c"
wayland-scanner client-header "$protocol_dir/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml" "$MULTISEAT_TEST_DIR/shortcut-inhibit.h"
wayland-scanner private-code "$protocol_dir/unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml" "$MULTISEAT_TEST_DIR/shortcut-inhibit.c"
cc -I"$MULTISEAT_TEST_DIR" "$source_dir/constraints.c" "$MULTISEAT_TEST_DIR/constraints.c" "$MULTISEAT_TEST_DIR/relative-pointer.c" "$MULTISEAT_TEST_DIR/activation.c" "$MULTISEAT_TEST_DIR/shortcut-inhibit.c" -o "$MULTISEAT_TEST_DIR/constraints" $(pkg-config --cflags --libs gtk+-3.0 wayland-client)

wayland-scanner client-header "$repo_dir/protocols/hyprland-lock-scope-v1.xml" "$MULTISEAT_TEST_DIR/lock-scope.h"
wayland-scanner private-code "$repo_dir/protocols/hyprland-lock-scope-v1.xml" "$MULTISEAT_TEST_DIR/lock-scope.c"
cc -I"$MULTISEAT_TEST_DIR" "$source_dir/lock-scope.c" "$MULTISEAT_TEST_DIR/lock-scope.c" -o "$MULTISEAT_TEST_DIR/scope-probe" $(pkg-config --cflags --libs wayland-client)

cc -I"$MULTISEAT_TEST_DIR" "$source_dir/lock-guard.c" "$MULTISEAT_TEST_DIR/lock-scope.c" -o "$MULTISEAT_TEST_DIR/lock-guard" $(pkg-config --cflags --libs wayland-client)
