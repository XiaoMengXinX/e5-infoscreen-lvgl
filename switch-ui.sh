#!/bin/sh
# Usage: switch-ui.sh lvgl | webkit
# Run on the device after uploading the binary to /usr/libexec/e5-infoscreen/ui.
set -e
dir=/usr/libexec/e5-infoscreen

case "$1" in
lvgl)
    [ -x $dir/ui ] || { echo "missing $dir/ui" >&2; exit 1; }
    [ -e $dir/session.webkit ] || cp -p $dir/session $dir/session.webkit
    cat > $dir/session.new <<'SESSION'
#!/bin/sh
TZ=$(cat /etc/TZ 2>/dev/null)
export TZ=${TZ:-UTC0}

# wait up to ten seconds for the api instance (8088 = 0x1F98, listening = 0A)
i=0
while ! grep -q ':1F98 00000000:0000 0A' /proc/net/tcp && [ $i -lt 100 ]; do /opt/e5/bin/busybox sleep 0.1; i=$((i + 1)); done

exec /usr/libexec/e5-infoscreen/ui
SESSION
    chmod 755 $dir/session.new
    mv $dir/session.new $dir/session
    ;;
webkit)
    [ -e $dir/session.webkit ] || { echo "missing $dir/session.webkit" >&2; exit 1; }
    cp -p $dir/session.webkit $dir/session.new
    mv $dir/session.new $dir/session
    ;;
*)
    echo "usage: $0 lvgl | webkit" >&2
    exit 1
    ;;
esac

/etc/init.d/e5-infoscreen restart
