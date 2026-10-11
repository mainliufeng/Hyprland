#!/usr/bin/env bash
set -euo pipefail
[[ $# == 4 ]] || { echo 'Usage: run.sh HYPRLAND HYPRTESTER PLUGIN CLIENT_DIRECTORY' >&2; exit 2; }
exec "$(dirname "${BASH_SOURCE[0]}")/sandbox.sh" multiseat "$@"
