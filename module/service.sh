#!/system/bin/sh
# KernelSU late_start service: start the rate-only guard daemon.
MODDIR=${0%/*}
DATA=/data/adb/scene_rate_guard
BIN="$MODDIR/bin/scene_guard.sh"

mkdir -p "$DATA" 2>/dev/null
chmod 700 "$DATA" 2>/dev/null
chmod 755 "$BIN" 2>/dev/null
rm -f "$DATA/stop" 2>/dev/null

if [ -r "$DATA/daemon.pid" ]; then
    old=$(cat "$DATA/daemon.pid" 2>/dev/null)
    case "$old" in
        ''|*[!0-9]*) ;;
        *)
            if grep -q 'scene_guard.sh' "/proc/$old/cmdline" 2>/dev/null; then
                exit 0
            fi
            ;;
    esac
fi

if command -v nohup >/dev/null 2>&1; then
    nohup /system/bin/sh "$BIN" daemon >/dev/null 2>&1 &
else
    /system/bin/sh "$BIN" daemon >/dev/null 2>&1 &
fi
exit 0
