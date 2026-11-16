#!/system/bin/sh
SKIPUNZIP=0

if [ "$ARCH" != "arm64" ]; then
    abort "! 潮汐仅支持 arm64 (aarch64) 架构设备"
fi

ui_print "- 正在安装 潮汐 (CPU Rate Limit 事件守护)"
ui_print "- 架构: $ARCH | 机制: epoll + inotify + netlink + timerfd"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/chaoxid" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

mkdir -p /data/adb/chaoxi
chmod 700 /data/adb/chaoxi

ui_print "- 安装完成，重启后自动守护（也可在 KernelSU 管理器打开 WebUI 配置）"
