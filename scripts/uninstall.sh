#!/usr/bin/env bash
# Run on a computer that reaches the device over SSH: switches the device's screen
# back to the web interface and removes what this interface left on it.
# Usage: ./uninstall.sh [user@host]        asks for the device when not given
set -e
here=$(cd "$(dirname "$0")" && pwd)

# The usual address is filled in to edit (bash 4 and later; macOS ships 3.2,
# where Enter alone takes it).
target=$1
if [ -z "$target" ]; then
    if [ "${BASH_VERSINFO[0]}" -ge 4 ]; then
        read -r -e -i root@192.168.9.1 -p 'SSH target: ' target || exit 1
    else
        read -r -p 'SSH target [root@192.168.9.1]: ' target || exit 1
    fi
    target=${target:-root@192.168.9.1}
fi

control=$(mktemp -d)
trap 'ssh -o ControlPath="$control/ssh" -O exit "$target" 2>/dev/null; rm -rf "$control"' EXIT
ssh="ssh -o ControlMaster=auto -o ControlPath=$control/ssh -o ControlPersist=60"

echo "Connecting to $target. If ssh asks, enter the device's password."
echo "Switching to the web interface, then removing this one."
$ssh "$target" "sh -s webkit" <"$here/switch-ui.sh"
$ssh "$target" "sh -s" <"$here/remove-ui.sh"
echo "done: uninstalled from $target"
