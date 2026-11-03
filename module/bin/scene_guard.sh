#!/system/bin/sh
# 潮汐 Lite · Scene Rate Guard backend
# Only manages sugov_ext up/down rate_limit_us. Never touches governor or freq.
# Test-only overrides: RL_DATA, RL_CPU_ROOT, RL_POWER_ROOT.

export LC_ALL=C

DATA=${RL_DATA:-/data/adb/scene_rate_guard}
CPU_ROOT=${RL_CPU_ROOT:-/sys/devices/system/cpu/cpufreq}
POWER_ROOT=${RL_POWER_ROOT:-/sys/class/power_supply}
CONF="$DATA/config.conf"
VALUES="$DATA/values.conf"
STATE="$DATA/state.conf"
LOG="$DATA/guard.log"
PID="$DATA/daemon.pid"
NODES="$DATA/nodes.$$"
MAX_RATE=20000
BOOT_WAIT=60
INTERVAL=2

mkdir -p "$DATA" 2>/dev/null
chmod 700 "$DATA" 2>/dev/null

log() {
    [ -d "$DATA" ] || return 0
    echo "$(date '+%m-%d %H:%M:%S') $*" >> "$LOG" 2>/dev/null
    if [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt 102400 ]; then
        tail -n 160 "$LOG" > "$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG"
    fi
}

cleanup_tmp() {
    rm -f "$NODES" "$NODES.s" "$CONF.tmp.$$" "$VALUES.tmp.$$" "$STATE.tmp.$$" 2>/dev/null
}

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -ge 0 ] 2>/dev/null
}

is_rate() {
    is_uint "$1" && [ "$1" -le "$MAX_RATE" ] 2>/dev/null
}

pid_is_daemon() {
    p_pid=$1
    is_uint "$p_pid" || return 1
    [ -d "/proc/$p_pid" ] || return 1
    grep -q 'scene_guard.sh' "/proc/$p_pid/cmdline" 2>/dev/null
}

read_val() {
    [ -r "$1" ] || { echo ""; return 1; }
    tr -cd '0-9' < "$1" 2>/dev/null | cut -c1-12
}

read_text() {
    [ -r "$1" ] || { echo ""; return 1; }
    cat "$1" 2>/dev/null | tr -d '\r\n' | cut -c1-64
}

read_conf() {
    ENABLED=0
    [ -f "$CONF" ] || return 0
    while IFS='=' read -r key value; do
        case "$key" in
            enabled) [ "$value" = "1" ] && ENABLED=1 || ENABLED=0 ;;
        esac
    done < "$CONF"
}

save_conf() {
    tmp="$CONF.tmp.$$"
    echo "enabled=$ENABLED" > "$tmp" 2>/dev/null || return 1
    chmod 600 "$tmp" 2>/dev/null
    mv -f "$tmp" "$CONF" 2>/dev/null
}

read_state() {
    FIXES=0
    LAST_APPLY=0
    [ -f "$STATE" ] || return 0
    while IFS='=' read -r key value; do
        case "$key" in
            fixes) is_uint "$value" && FIXES=$value ;;
            last_apply) is_uint "$value" && LAST_APPLY=$value ;;
        esac
    done < "$STATE"
}

save_state() {
    tmp="$STATE.tmp.$$"
    {
        echo "fixes=$FIXES"
        echo "last_apply=$LAST_APPLY"
    } > "$tmp" 2>/dev/null || return 1
    chmod 600 "$tmp" 2>/dev/null
    mv -f "$tmp" "$STATE" 2>/dev/null
}

charging_state() {
    for p in "$POWER_ROOT"/*; do
        [ -d "$p" ] || continue
        case "${p##*/}" in
            battery|*bms*|*battery*) continue ;;
        esac
        [ -r "$p/online" ] && [ "$(cat "$p/online" 2>/dev/null)" = "1" ] && {
            echo 1
            return 0
        }
    done
    for p in "$POWER_ROOT"/*; do
        [ -r "$p/status" ] || continue
        case "$(cat "$p/status" 2>/dev/null)" in
            Charging|Full) echo 1; return 0 ;;
        esac
    done
    echo 0
}

battery_capacity() {
    for p in "$POWER_ROOT"/*; do
        [ -r "$p/capacity" ] || continue
        case "${p##*/}" in
            *battery*|bms) read_val "$p/capacity"; return 0 ;;
        esac
    done
    echo ""
}

line_for() {
    file=$1
    idx=$2
    [ -f "$file" ] || { echo ""; return 0; }
    sed -n "$((idx + 1))p" "$file" 2>/dev/null | tr -d '\r\n' | cut -c1-32
}

desired_rate() {
    line_for "$VALUES" "$1" | tr -cd '0-9'
}

# NODES: maxcpu|policy|up_path|down_path|cpus|governor
discover() {
    : > "$NODES" 2>/dev/null
    for p in "$CPU_ROOT"/policy[0-9]*; do
        [ -d "$p" ] || continue
        gov=$(read_text "$p/scaling_governor")
        up=""
        down=""
        for dir in "$p/$gov" "$p/sugov_ext" "$p/schedutil" "$p"/*; do
            if [ -f "$dir/up_rate_limit_us" ] && [ -f "$dir/down_rate_limit_us" ]; then
                up="$dir/up_rate_limit_us"
                down="$dir/down_rate_limit_us"
                break
            fi
        done
        [ -n "$up" ] && [ -n "$down" ] || continue

        maxcpu=$(read_val "$p/cpuinfo_max_freq")
        [ -n "$maxcpu" ] || maxcpu=0
        cpus=$(cat "$p/affected_cpus" 2>/dev/null)
        [ -n "$cpus" ] || cpus=$(cat "$p/related_cpus" 2>/dev/null)
        cpus=$(echo "$cpus" | tr '\n\t' '  ' | tr -s ' ' | sed 's/^ //;s/ $//')
        printf '%s|%s|%s|%s|%s|%s\n' "$maxcpu" "$p" "$up" "$down" "$cpus" "$gov" >> "$NODES"
    done

    if [ -s "$NODES" ]; then
        sort -n -t'|' -k1,1 "$NODES" > "$NODES.s" 2>/dev/null && mv -f "$NODES.s" "$NODES"
    fi
    COUNT=$(wc -l < "$NODES" 2>/dev/null | tr -d ' ')
    [ -n "$COUNT" ] || COUNT=0
}

write_num() {
    path=$1
    value=$2
    [ -e "$path" ] || return 1
    printf '%s\n' "$value" > "$path" 2>/dev/null || return 1
    [ "$(read_val "$path")" = "$value" ]
}

write_pair() {
    up_path=$1
    down_path=$2
    value=$3
    write_num "$up_path" "$value" || return 1
    write_num "$down_path" "$value" || return 1
}

enforce() {
    mode=${1:-check}
    [ "$COUNT" -gt 0 ] || return 0
    changed=0
    failed=0
    i=0
    while IFS='|' read -r maxcpu policy up_path down_path cpus gov; do
        want=$(desired_rate "$i")
        i=$((i + 1))
        [ -n "$want" ] || continue
        is_rate "$want" || continue
        cur_up=$(read_val "$up_path")
        cur_down=$(read_val "$down_path")
        if [ "$mode" = "force" ] || [ "$cur_up" != "$want" ] || [ "$cur_down" != "$want" ]; then
            if write_pair "$up_path" "$down_path" "$want"; then
                if [ "$cur_up" != "$want" ] || [ "$cur_down" != "$want" ]; then
                    log "rate ${policy##*/} ${cur_up:-?}/${cur_down:-?} -> $want"
                    FIXES=$((FIXES + 1))
                    LAST_APPLY=$(date +%s)
                    changed=1
                fi
            else
                failed=1
                log "rate write failed ${policy##*/} -> $want"
            fi
        fi
    done < "$NODES"
    [ "$changed" = "1" ] && STATE_DIRTY=1
    [ "$failed" = "1" ] && return 1
    return 0
}

cmd_status() {
    read_conf
    read_state
    discover
    CHARGING=$(charging_state)
    BATTERY=$(battery_capacity)
    daemon=0
    if [ -r "$PID" ]; then
        pid=$(cat "$PID" 2>/dev/null)
        pid_is_daemon "$pid" && daemon=1
    fi
    device=$(getprop ro.product.model 2>/dev/null)
    soc=$(getprop ro.soc.model 2>/dev/null)
    configured=0
    [ -f "$CONF" ] && configured=1

    if [ "$COUNT" -gt 0 ]; then
        echo "ok=1"
    else
        echo "ok=0"
        echo "error=no_rate_nodes"
    fi
    echo "enabled=$ENABLED"
    echo "configured=$configured"
    echo "daemon=$daemon"
    echo "charging=$CHARGING"
    echo "battery=$BATTERY"
    echo "fixes=$FIXES"
    echo "last_apply=$LAST_APPLY"
    echo "count=$COUNT"
    echo "device=$device"
    echo "soc=$soc"
    echo "max_rate=$MAX_RATE"
    echo "interval=$INTERVAL"

    i=0
    while IFS='|' read -r maxcpu policy up_path down_path cpus gov; do
        cur_up=$(read_val "$up_path")
        cur_down=$(read_val "$down_path")
        want=$(desired_rate "$i")
        [ -n "$want" ] || want=$cur_up
        echo "c${i}_policy=${policy##*/}"
        echo "c${i}_cpus=$cpus"
        echo "c${i}_max=$maxcpu"
        echo "c${i}_gov=$gov"
        echo "c${i}_up_path=$up_path"
        echo "c${i}_down_path=$down_path"
        echo "c${i}_up=$cur_up"
        echo "c${i}_down=$cur_down"
        echo "c${i}_desired=$want"
        i=$((i + 1))
    done < "$NODES"
    cleanup_tmp
}

cmd_apply() {
    read_conf
    read_state
    discover
    if [ "$COUNT" -eq 0 ]; then
        echo "ok=0"
        echo "error=no_rate_nodes"
        return 1
    fi
    if [ "$#" -ne "$COUNT" ]; then
        echo "ok=0"
        echo "error=count_mismatch"
        echo "detail=expected_$COUNT"
        return 1
    fi
    for value in "$@"; do
        if ! is_rate "$value"; then
            echo "ok=0"
            echo "error=invalid_rate"
            echo "detail=$value"
            return 1
        fi
    done

    tmp="$VALUES.tmp.$$"
    : > "$tmp" 2>/dev/null
    for value in "$@"; do
        echo "$value" >> "$tmp"
    done
    chmod 600 "$tmp" 2>/dev/null
    if ! mv -f "$tmp" "$VALUES" 2>/dev/null; then
        echo "ok=0"
        echo "error=config_write_failed"
        return 1
    fi

    ENABLED=1
    if ! save_conf; then
        echo "ok=0"
        echo "error=config_write_failed"
        return 1
    fi
    if ! enforce force; then
        ENABLED=0
        save_conf
        echo "ok=0"
        echo "error=write_failed"
        echo "detail=see_guard_log"
        return 1
    fi
    save_state
    echo "ok=1"
    echo "enabled=1"
    echo "count=$COUNT"
    cleanup_tmp
}

cmd_pause() {
    read_conf
    ENABLED=0
    save_conf || { echo "ok=0"; echo "error=config_write_failed"; return 1; }
    log "guard paused"
    echo "ok=1"
    echo "enabled=0"
    cleanup_tmp
}

cmd_resume() {
    read_conf
    ENABLED=1
    save_conf || { echo "ok=0"; echo "error=config_write_failed"; return 1; }
    log "guard resumed"
    echo "ok=1"
    echo "enabled=1"
    cleanup_tmp
}

cmd_log() {
    if [ -s "$LOG" ]; then
        tail -n 80 "$LOG"
    else
        echo "还没有日志。"
    fi
}

wait_boot_minute() {
    while [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ]; do
        sleep 2
    done
    up=$(cut -d. -f1 /proc/uptime 2>/dev/null)
    is_uint "$up" || up=0
    if [ "$up" -lt "$BOOT_WAIT" ]; then
        sleep $((BOOT_WAIT - up))
    fi
}

daemon_main() {
    if [ -r "$PID" ]; then
        old=$(cat "$PID" 2>/dev/null)
        if pid_is_daemon "$old" && [ "$old" != "$$" ]; then
            echo "already running: $old"
            return 0
        fi
    fi
    echo $$ > "$PID" 2>/dev/null
    trap 'cleanup_tmp; rm -f "$PID" 2>/dev/null; exit 0' 1 2 15
    trap 'cleanup_tmp; rm -f "$PID" 2>/dev/null' 0

    wait_boot_minute
    log "daemon started pid=$$"
    read_conf
    read_state
    discover
    LAST_CHARGE=$(charging_state)
    LOW_BATTERY=0
    STATE_DIRTY=0
    LAST_SAVE=$(date +%s)
    loop=0
    first=1

    while :; do
        if [ -f "$DATA/stop" ]; then
            rm -f "$DATA/stop" 2>/dev/null
            log "daemon stop requested"
            exit 0
        fi
        read_conf
        if [ "$ENABLED" = "1" ]; then
            [ "$COUNT" -eq 0 ] && discover
            [ $((loop % 30)) -eq 0 ] && discover
            now_charge=$(charging_state)
            if [ "$now_charge" != "$LAST_CHARGE" ]; then
                log "charger ${LAST_CHARGE} -> ${now_charge}, re-pin"
                LAST_CHARGE=$now_charge
                enforce force
                sleep 0.30
                enforce force
                sleep 0.60
                enforce force
                STATE_DIRTY=1
            elif [ "$first" = "1" ]; then
                enforce check
                first=0
            else
                enforce check || sleep 2
            fi

            if [ $((loop % 15)) -eq 0 ]; then
                cap=$(battery_capacity)
                if is_uint "$cap" && [ "$cap" -le 5 ] && [ "$LOW_BATTERY" = "0" ]; then
                    log "battery=${cap}%, re-pin"
                    LOW_BATTERY=1
                    enforce force
                    STATE_DIRTY=1
                elif is_uint "$cap" && [ "$cap" -gt 7 ]; then
                    LOW_BATTERY=0
                fi
            fi
        else
            LAST_CHARGE=$(charging_state)
            first=1
        fi

        if [ "$STATE_DIRTY" = "1" ]; then
            now=$(date +%s)
            if [ $((now - LAST_SAVE)) -ge 3 ]; then
                save_state
                STATE_DIRTY=0
                LAST_SAVE=$now
            fi
        fi
        loop=$((loop + 1))
        sleep "$INTERVAL"
    done
}

case "$1" in
    daemon) daemon_main ;;
    status) cmd_status ;;
    apply) shift; cmd_apply "$@" ;;
    pause) cmd_pause ;;
    resume) cmd_resume ;;
    log) cmd_log ;;
    *)
        echo "usage: $0 {daemon|status|apply v1 [v2 ...]|pause|resume|log}"
        exit 2
        ;;
esac
