# 潮汐 Lite · Scene Rate Guard

只管理 **`up_rate_limit_us` / `down_rate_limit_us`** 的 KernelSU 模块，目标设备 **Redmi K80 Ultra（dali）/ 天玑 9400+ / HyperOS 3**。

> 这次不再写 governor，不再写 `scaling_min_freq` / `scaling_max_freq`。
> 只做一件事：把三簇的升/降频限幅按住，避免 Scene 的参数被系统或电源 HAL 覆盖。

## 为什么不是 v1.1 的 rate_limit_guard

这台机器的实际节点是：

```text
/sys/devices/system/cpu/cpufreq/policy0/sugov_ext/up_rate_limit_us
/sys/devices/system/cpu/cpufreq/policy0/sugov_ext/down_rate_limit_us
```

不是 `rate_limit_us`。  
v1.1 还在同时写 governor 和频率上下限，其中频率写入被内核拒绝，于是整个应用报 `write_failed`；同时它把 Scene 的低频上限抬回满频，功耗自然更高。

潮汐 Lite 只碰这 6 个节点：

```text
policy0/sugov_ext/up_rate_limit_us
policy0/sugov_ext/down_rate_limit_us
policy4/sugov_ext/up_rate_limit_us
policy4/sugov_ext/down_rate_limit_us
policy7/sugov_ext/up_rate_limit_us
policy7/sugov_ext/down_rate_limit_us
```

## 安装

1. 先禁用旧的 `rate_limit_guard`（本机已禁用）。
2. KernelSU 管理器 → 模块 → 安装 `scene_rate_guard-v1.0.0.zip`。
3. 重启手机。
4. 打开模块 WebUI。
5. 点「Scene 省电 / 均衡 / 性能」之一填入，或手动改三簇数值。
6. 点「应用并守护」。

守护进程会在开机满 60 秒后开始工作；之后每 2 秒做一次轻量比对，只在值被改写时重写。

## 触发与保活

- **开机满 60 秒**：开始第一次校准；
- **充电器拔出/接入**：检测 `online/status` 变化，立即做 3 连写（0s / 0.3s / 0.9s）；
- **息屏→亮屏**：由 2 秒校验兜底，覆盖亮屏后系统重写；
- **电量 ≤5%**：每 30 秒检查一次，跨过门槛时强制写一次；7% 以上复位触发标记；
- **日常**：每 2 秒读取 6 个节点，只有值不一致才写，稳定时不产生写入。

不做 governor / 频率写入，所以不会再把频率上限抬回去，也不会和 Scene 的 governor 抢控制权。

## Scene 预设

WebUI 会尝试读取：

```text
/data/data/com.omarea.vtools/files/profile.json
```

解析其中的：

- `powersave` → Scene 省电（16000 / 12000 / 4000）
- `balance` → Scene 均衡（8000 / 4000 / 1000）
- `performance` → Scene 性能（2000 / 1000 / 400）

点一个预设会填入三簇目标值，再点应用。Scene 的 `fast` 方案使用 `performance` governor、没有 rate 字段，所以不会出现在预设里。

如果 Scene 未安装或 profile 不可读，预设区会自动隐藏，仍可手动设置。

## 外观

- 保留 5 套莫奈取色；
- 新增 **液态玻璃** 风格：半透明玻璃卡片、极光背景、高光边缘、轻微漂移动画；
- 保留壁纸 4 套 + 自定义上传；
- 保留文楷 / 素宋 / 圆体 / 等宽字体栈；
- **已去掉震动**：没有 `navigator.vibrate`、没有触感设置。按钮仍有按压回弹，滑条仍有加宽、气泡和跟手动画；
- 滑条使用 `requestAnimationFrame` 合帧，减少拖动时的布局抖动。

## 运行数据

`/data/adb/scene_rate_guard/`：

- `config.conf`：是否启用；
- `values.conf`：三簇目标值（一行一个，同时写入 up/down）；
- `state.conf`：回写次数、最近校准时间；
- `guard.log`：日志；
- `daemon.pid`：守护进程 PID。

## 排错

**显示“没有找到 up/down rate_limit_us 节点”**

```sh
su -c 'find /sys/devices/system/cpu/cpufreq -name "*rate_limit_us"'
su -c 'for p in /sys/devices/system/cpu/cpufreq/policy*; do echo ===$p; cat $p/scaling_governor; ls -l $p/sugov_ext 2>/dev/null; done'
```

**写入失败**

```sh
su -c 'tail -n 50 /data/adb/scene_rate_guard/guard.log'
su -c 'dmesg | grep avc | tail -30'
```

**Scene 预设不显示**

确认 Scene 仍安装，并且可以读：

```sh
su -c 'ls -l /data/data/com.omarea.vtools/files/profile.json'
```

## 已验证

在真实 K80 Ultra 上通过 `su` 验证：

- `up/down_rate_limit_us` 可写、可回读；
- 写入 Scene 省电的 `16000 / 12000 / 4000` 后，守护循环在系统重置为 `10/10` 后重新写回；
- 实际发现 `policy7` 会被快速重置，2 秒守护能及时恢复；
- 通过临时数据目录验证 apply / status / pause / resume / 日志。

WebUI 用 390px 移动视口和 1200px 桌面视口完成布局、Scene 预设、滑条、主题切换与液态玻璃检查。

## 许可

模块代码可自由修改用于个人设备。字体使用系统已有字体栈，不分发第三方字体文件。
