#!/system/bin/sh
MODDIR=${0%/*}
DATA=/data/adb/chaoxi
BIN="$MODDIR/bin/chaoxid"

mkdir -p "$DATA" 2>/dev/null
chmod 700 "$DATA" 2>/dev/null
chmod 755 "$BIN" 2>/dev/null

# 清理残留 PID 并防止重复实例（二进制内部亦通过 flock 保证单实例）
if [ -r "$DATA/daemon.pid" ]; then
    old=$(cat "$DATA/daemon.pid" 2>/dev/null)
    case "$old" in
        ''|*[!0-9]*) ;;
        *)
            if grep -q 'chaoxid' "/proc/$old/cmdline" 2>/dev/null; then
                exit 0
            fi
            ;;
    esac
fi

# 由二进制内部执行 --bg 双重 fork、cgroup 逃逸与 oom_score_adj=-900 保护（避免随前端被回收）
"$BIN" daemon --bg
exit 0
