#!/bin/sh
# Run on a computer that reaches the device over SSH.
# Usage: ./install.sh [user@host]          install this interface and switch to it
#        ./install.sh [user@host] webkit   switch the device back to the web interface
# The host defaults to root@192.168.8.1.
set -e
here=$(cd "$(dirname "$0")" && pwd)
target=${1:-root@192.168.8.1}
mode=${2:-lvgl}
dir=/usr/libexec/e5-infoscreen

case "$mode" in
lvgl | webkit) ;;
*)
    echo "usage: $0 [user@host] [lvgl | webkit]" >&2
    exit 1
    ;;
esac

# Next to this script in a release, under out/ in a source checkout.
binary=$here/e5-infoscreen-ui
[ -f "$binary" ] || binary=$here/../out/e5-infoscreen-ui
if [ "$mode" = lvgl ] && [ ! -f "$binary" ]; then
    echo "missing e5-infoscreen-ui: download a release or run python3 build.py" >&2
    exit 1
fi

# One SSH connection for both steps, so a password is asked for once. The files go
# through ssh itself: the device has no SFTP server for scp.
control=$(mktemp -d)
trap 'ssh -o ControlPath="$control/ssh" -O exit "$target" 2>/dev/null; rm -rf "$control"' EXIT
ssh="ssh -o ControlMaster=auto -o ControlPath=$control/ssh -o ControlPersist=60"

if [ "$mode" = lvgl ]; then
    echo "uploading to $target"
    $ssh "$target" "cat > $dir/ui.new" <"$binary"
    $ssh "$target" "mv $dir/ui.new $dir/ui && sh -s lvgl" <"$here/switch-ui.sh"
else
    $ssh "$target" "sh -s webkit" <"$here/switch-ui.sh"
fi
echo "done: $mode interface on $target"
