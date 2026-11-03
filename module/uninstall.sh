#!/system/bin/sh
# Stop the daemon before the module directory disappears.
DATA=/data/adb/scene_rate_guard
if [ -r "$DATA/daemon.pid" ]; then
    pid=$(cat "$DATA/daemon.pid" 2>/dev/null)
    case "$pid" in
        ''|*[!0-9]*) ;;
        *)
            if grep -q 'scene_guard.sh' "/proc/$pid/cmdline" 2>/dev/null; then
                kill "$pid" 2>/dev/null
            fi
            ;;
    esac
fi
rm -rf "$DATA"
