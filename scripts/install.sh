#!/usr/bin/env bash
# Run on a computer that reaches the device over SSH: installs this interface and
# switches the device's screen to it. uninstall.sh switches back.
# Usage: ./install.sh [address|user@host]   asks for the device when not given
set -e
here=$(cd "$(dirname "$0")" && pwd)
dir=/usr/libexec/e5-infoscreen

# Next to this script in a release, under out/ in a source checkout.
binary=$here/e5-infoscreen-ui
[ -f "$binary" ] || binary=$here/../out/e5-infoscreen-ui
if [ ! -f "$binary" ]; then
    echo "missing e5-infoscreen-ui: download a release or run python3 build.py" >&2
    exit 1
fi

# The usual address is filled in to edit (bash 4 and later; macOS ships 3.2,
# where Enter alone takes it). The device signs in as root, so an address alone
# gets that user.
target=$1
if [ -z "$target" ]; then
    if [ "${BASH_VERSINFO[0]}" -ge 4 ]; then
        read -r -e -i 192.168.9.1 -p 'Device address: ' target || exit 1
    else
        read -r -p 'Device address [192.168.9.1]: ' target || exit 1
    fi
    target=${target:-192.168.9.1}
fi
case $target in *@*) ;; *) target=root@$target ;; esac

control=$(mktemp -d)
trap 'ssh -o ControlPath="$control/ssh" -O exit "$target" 2>/dev/null; rm -rf "$control"' EXIT
ssh="ssh -o ControlMaster=auto -o ControlPath=$control/ssh -o ControlPersist=60"

echo "Connecting to $target. If ssh asks, enter the device's password."
echo "Uploading the program, then installing it and restarting the screen."
$ssh "$target" "cat > $dir/ui.new" <"$binary"
# switch-ui stays on the device for the screens' own switches between interfaces.
$ssh "$target" "cd $dir && cat > switch-ui.new && chmod 755 switch-ui.new && mv switch-ui.new switch-ui && mv ui.new ui && ./switch-ui lvgl" <"$here/switch-ui.sh"
echo "done: installed on $target"
