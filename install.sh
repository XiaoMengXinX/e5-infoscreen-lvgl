#!/bin/sh
# Usage: sh install.sh
# Installs the e5-infoscreen-ui next to this script as the screen interface;
# sh switch-ui.sh webkit puts the web interface back.
set -e
here=$(cd "$(dirname "$0")" && pwd)
dir=/usr/libexec/e5-infoscreen

[ -d $dir ] || { echo "missing $dir: install e5-infoscreen first" >&2; exit 1; }
[ -f "$here/e5-infoscreen-ui" ] || { echo "missing $here/e5-infoscreen-ui" >&2; exit 1; }
cp "$here/e5-infoscreen-ui" $dir/ui.new
chmod 755 $dir/ui.new
mv $dir/ui.new $dir/ui
sh "$here/switch-ui.sh" lvgl
