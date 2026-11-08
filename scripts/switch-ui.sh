#!/bin/sh
# Usage: switch-ui.sh lvgl | webkit
# Run on the device after uploading the binary to /usr/libexec/e5-infoscreen/ui.
set -e
dir=/usr/libexec/e5-infoscreen

case "$1" in
lvgl)
    [ -f $dir/ui ] || { echo "missing $dir/ui" >&2; exit 1; }
    chmod 755 $dir/ui
    [ -e $dir/session.webkit ] || cp -p $dir/session $dir/session.webkit
    cat > $dir/session.new <<'SESSION'
#!/bin/sh
TZ=$(cat /etc/TZ 2>/dev/null)
export TZ=${TZ:-UTC0}

# wait up to ten seconds for the api instance (8088 = 0x1F98, listening = 0A)
i=0
while ! grep -q  ':1F98 00000000:0000 0A' /proc/net/tcp && [ $i -lt 100 ]; do /opt/e5/bin/busybox sleep 0.1; i=$((i + 1)); done

exec /usr/libexec/e5-infoscreen/ui
SESSION
    chmod 755 $dir/session.new
    mv $dir/session.new $dir/session
    # The SIM toolkit tells the two cards apart only once the modem is in its
    # dual-SIM channel mapping, set at boot before the SIMs start.
    cat > /etc/init.d/e5-infoscreen-sim <<'SERVICE'
#!/bin/sh /etc/rc.common
START=22
USE_PROCD=1

start_service() {
	procd_open_instance
	procd_set_param command /usr/libexec/e5-infoscreen/ui --map-sim-channels
	procd_set_param stderr 1
	procd_close_instance
}
SERVICE
    chmod 755 /etc/init.d/e5-infoscreen-sim
    # On unless the screen's SIM toolkit setting turned it off.
    if [ "$(uci -q get e5-plugin-stk.settings.enabled || uci -q get e5-infoscreen.main.stk)" = 0 ]; then
        /etc/init.d/e5-infoscreen-sim disable
    else
        /etc/init.d/e5-infoscreen-sim enable
    fi
    ;;
webkit)
    [ -e $dir/session.webkit ] || { echo "missing $dir/session.webkit" >&2; exit 1; }
    cp -p $dir/session.webkit $dir/session.new
    mv $dir/session.new $dir/session
    if [ -e /etc/init.d/e5-infoscreen-sim ]; then
        /etc/init.d/e5-infoscreen-sim disable
        rm /etc/init.d/e5-infoscreen-sim
    fi
    ;;
*)
    echo "usage: $0 lvgl | webkit" >&2
    exit 1
    ;;
esac

/etc/init.d/e5-infoscreen restart
