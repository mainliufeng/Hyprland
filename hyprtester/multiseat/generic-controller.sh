#!/usr/bin/env bash
set -euo pipefail
[[ $# == 1 ]] || { echo 'Usage: generic-controller.sh /absolute/path/to/Hyprland' >&2; exit 2; }
exec "$(dirname "${BASH_SOURCE[0]}")/sandbox.sh" controller "$1"
