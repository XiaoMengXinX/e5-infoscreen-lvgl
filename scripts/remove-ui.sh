#!/bin/sh
# Run on the device after switch-ui.sh webkit: removes this interface's program and
# what it kept, leaving the web interface as it was.
dir=/usr/libexec/e5-infoscreen
plugins=/usr/share/e5-infoscreen/www/plugins
rm -f $dir/ui $dir/switch-ui $dir/session.webkit /tmp/e5-stk.log /tmp/e5-stk.log.old
rm -f /etc/e5-infoscreen/ui.backup
uci -q delete e5-infoscreen.main.lock_keys
uci -q delete e5-infoscreen.main.ui_update_auto
# The web interface's SIM toolkit and speed test apps share the menus, the
# sessions, the settings and the fetched speed test.
if [ ! -e $plugins/stk ]; then
    rm -rf /etc/e5-infoscreen/stk /tmp/e5-stk
    uci -q delete e5-infoscreen.main.stk
fi
if [ ! -e $plugins/speedtest ]; then
    uci -q delete e5-infoscreen.main.speedtest_backend
    rm -f /usr/bin/globalspeedtest-go
fi
uci commit e5-infoscreen
