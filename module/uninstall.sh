#!/system/bin/sh
MODDIR=${0%/*}
DATA=/data/adb/chaoxi
BIN="$MODDIR/bin/chaoxid"

if [ -x "$BIN" ]; then
    "$BIN" stop >/dev/null 2>&1
elif [ -r "$DATA/daemon.pid" ]; then
    old=$(cat "$DATA/daemon.pid" 2>/dev/null)
    [ -n "$old" ] && kill "$old" 2>/dev/null
fi

sleep 0.2
chmod 0664 /sys/devices/system/cpu/cpufreq/policy*/sugov_ext/*_rate_limit_us 2>/dev/null
chmod 0664 /sys/devices/system/cpu/cpufreq/policy*/schedutil/*_rate_limit_us 2>/dev/null
rm -rf "$DATA" 2>/dev/null
exit 0
