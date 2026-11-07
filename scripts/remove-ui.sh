#!/bin/sh
# Run on the device after switch-ui.sh webkit: removes this interface's program and
# what it kept, leaving the web interface as it was.
dir=/usr/libexec/e5-infoscreen
rm -f $dir/ui $dir/session.webkit
rm -rf /etc/e5-infoscreen/stk /tmp/e5-stk /tmp/e5-stk.log /tmp/e5-stk.log.old
# the speed test's backend, the * and # unlock, the SIM toolkit, and the speed
# test it may have fetched
uci -q delete e5-infoscreen.main.speedtest_backend
uci -q delete e5-infoscreen.main.lock_keys
uci -q delete e5-infoscreen.main.stk
uci commit e5-infoscreen
rm -f /usr/bin/globalspeedtest-go
