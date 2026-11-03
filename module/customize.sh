#!/system/bin/sh
# KernelSU / Magisk installer hook.
SKIPUNZIP=0

ui_print " "
ui_print "  潮汐 Lite · Scene Rate Guard"
ui_print "  只管理 sugov_ext up/down rate_limit_us"
ui_print " "

set_perm "$MODPATH/module.prop" 0 0 0644
set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/customize.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

ui_print "- 已安装。重启后在 KernelSU 模块页打开 WebUI。"
ui_print "- 首次进入可用 Scene 预设填入省电/均衡/性能值。"
