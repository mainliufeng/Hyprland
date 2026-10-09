#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "$source_dir/../.." && pwd)
binary=${1:-$repo_dir/build-multiseat/Hyprland}
[[ -x $binary ]] || { echo 'Built compositor required' >&2; exit 1; }
artifacts=$(mktemp -d /tmp/hyprland-controller.XXXXXX)
mkdir -m700 "$artifacts/home"
echo "Isolated artifacts: $artifacts"
exec nice -n 15 bwrap --unshare-all --die-with-parent --new-session \
  --ro-bind / / --dev /dev --proc /proc --tmpfs /run --tmpfs /tmp \
  --dir /dev/dri --dev-bind /dev/dri/renderD128 /dev/dri/renderD128 \
  --bind "$artifacts" /tmp/t --setenv HOME /tmp/t/home --setenv TMPDIR /tmp/t \
  --setenv HYPRLAND_TEST_SANDBOX 1 --setenv MULTISEAT_HYPRLAND "$binary" \
  --unsetenv HYPRLAND_INSTANCE_SIGNATURE --unsetenv WAYLAND_DISPLAY \
  --unsetenv WAYLAND_SOCKET --unsetenv DISPLAY --unsetenv DBUS_SESSION_BUS_ADDRESS \
  -- /usr/bin/python3 "$source_dir/generic-controller.py"
