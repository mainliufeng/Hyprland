#!/usr/bin/env bash
set -euo pipefail
[[ ${HYPRLAND_TEST_SANDBOX:-} == 1 && ! -e /dev/input && ! -e /dev/dri/card0 ]] || { echo 'Private device sandbox required' >&2; exit 2; }
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$source_dir/fixtures.sh"
case "${1:-}" in
    routing) "$HYPRLAND_TEST_PYTHON" "$source_dir/routing.py" ;;
    controller) "$HYPRLAND_TEST_PYTHON" "$source_dir/generic-controller.py" ;;
    multiseat)
        "$HYPRLAND_TEST_PYTHON" "$source_dir/probe.py"
        "$HYPRLAND_TEST_PYTHON" "$source_dir/single-seat.py" keyboardModifiersMergedOnFocus pointerWarp xdgInteractive popupOpacityInheritsParentFade xdgActivationSerial
        ;;
    dispatcher) "$HYPRLAND_TEST_PYTHON" "$source_dir/dispatcher.py" ;;
    lifecycle) "$HYPRLAND_TEST_PYTHON" "$source_dir/lifecycle.py" ;;
    single-seat) "$HYPRLAND_TEST_PYTHON" "$source_dir/single-seat.py" "${@:2}" ;;
    *) echo 'Unknown isolated suite' >&2; exit 2 ;;
esac
