# 潮汐 · Tide（CPU Rate Limit 事件守护模块）

一款基于 **Linux 内核事件机制（`epoll` + `inotify` + `Netlink` + `timerfd`）** 与 **静态 C++17** 实现的 KernelSU 模块，专注解决多集群 CPU `up_rate_limit_us` / `down_rate_limit_us` 参数在**日常应用切换提频结束、拔插充电器、低电量、长时间息屏再亮屏及调频器切换**后被系统或电源管理服务覆盖回 `10us` 等默认值的问题。

模块内置 **莫奈动态取色 WebUI**，支持自定义壁纸与字体，并支持将配置**直接设为系统与模块默认参数，同时统一覆写 Scene 调度策略档位**，在极低后台占用与零争抢的前提下确保速率参数长期稳定生效。

---

## 核心调控逻辑（v1.2.0 架构）

针对实测中“完全关闭节点监听会漏防系统在切换应用后下发的 `10us` 覆盖，而全时段无差别死磕系统 `0us` 提频又会导致剧烈功耗上升”的两难问题，**潮汐 v1.2.0** 采用 **「阻塞零轮询 + 提频放行 + 重置瞬时恢复 + 根 cgroup 抗回收」** 的精准事件引擎：

1. **全事件零轮询与进程防掉线架构（`chaoxid`）**
   - 主循环完全阻塞在内核 `epoll_wait(..., -1)` 上等待中断事件，日常静态期 0 唤醒、常驻内存 ~`500 KB` 级。
   - **独立守护与防被杀（`--bg` + 根 cgroup 逃逸）**：无论是开机脚本还是 WebUI 拉起，`chaoxid` 均会执行双重 `fork`、`setsid()`、stdio 隔离，并将自身移出应用受限 memcg/top-app cgroup 至根级 `/dev/memcg`、`/dev/cpuctl`、`/dev/cpuset` 与 `/sys/fs/cgroup/uid_0/pid_*`，同时设置 `oom_score_adj = -900`，彻底杜绝跟随前端应用一起被系统冻结或回收掉线。
   - 仅定位并写入各集群调频器目录（如 `sugov_ext` / `schedutil`）下的 `up_rate_limit_us` 与 `down_rate_limit_us`，**绝不修改 `scaling_governor` 或 `scaling_min_freq` / `scaling_max_freq`**。

2. **三档覆盖防护模式（默认：`smart` 智能低耗守卫）**
   - **`smart`（智能低耗守卫 · 默认推荐）**
     - **节点监控 + 三重静默滤波**：在保持 6 节点 `inotify` 监控及时捕获任意场景改写的同时，加入三层功耗控制：
       1. **`220ms` 批写入合并**：把系统一次多簇 6 节点连续改写合并成单次校验，避免无意义的多次唤醒；
       2. **`80ms` 自写入回声屏蔽**：自身写回参数后自动忽略自触发 `inotify` 事件，消除自激震荡；
       3. **智能区分“短时提频”与“系统重置”**：当系统/电源 HAL 处于活跃提频状态（`up == 0`，如 `0/0` 或 `0/6000`）时**不接入争抢**；若 `2500ms` 内出现超过 3 波连续活跃写入，**临时摘除节点监听 `2.2s` 退出高耗区间**，冷静后复查一次；一旦检测到系统写回 `10/10`、`1000/1000` 等非零默认重置值（含切换应用退出、拔插充电、低电量、亮屏及息屏 `700ms` 后残留 `0/0`、调频器目录重建），**`220ms` 内立即精准写回并迅速重回 `epoll_wait` 深睡**。
     - **亮度渐变防抖与低 I/O**：点亮态亮度平滑变化时自动临时屏蔽背光监听 `1800–2000ms`，消除 70Hz 渐变唤醒；状态文件落盘节流至 `300s`（5 分钟）；`4s` 内同簇遭遇真竞争超 3 次自动触发 `10s` 冷却防死磕。
   - **`scene`（Scene 档位自适应 · 智能守卫）**
     - 同样运行完整的 `10us` 瞬时恢复与 `0us` 提频避让引擎，同时读取并缓存 Scene `profile.json` 的 `powersave` / `balance` / `performance` 三档速率：当各档位数值有区分且检测到外部 Scene 切换档位时自动贴合当前激活档位，再在其后出现系统 `10us` 重置时精准还原当前激活档位；如外部修改 `profile.json`（自动过滤自身写入后 `3000ms` 窗口）亦会自动同步。
   - **`lock`（强力实时锁定）**
     - 连系统短时 `0us` 提频也会强制写回（适合严格固定速率压测），同样配备 `80ms` 批合并、自写入回声屏蔽与 `2.5s` 超 4 波争抢临时退出节点监听 `5s` 的熔断机制。

3. **系统 · 模块默认 · Scene 策略统一写入**
   - 前端点击写入或执行 `chaoxid apply ... --scheme <none|powersave|balance|performance|all>` 时：
     1. 立即写入内核 `sysfs` 并更新守护内存目标；
     2. 同步持久化至 `/data/adb/chaoxi/config.conf` 成为系统与模块默认参数；
     3. 如指定 Scene 档位，同步保留原始属主与权限原子修改 Scene `profile.json` 对应策略块及 `objects/<scheme>` 持久化对象，确保**系统当前值、模块默认值与 Scene 策略值三方完全一致**。

---

## 仓库目录结构

```text
.
├── src/
│   └── chaoxid.cpp          # C++17 核心事件守护进程、cgroup 防护与 Scene 写穿引擎
├── module/
│   ├── module.prop          # KernelSU 模块元数据 (id=chaoxi)
│   ├── customize.sh         # 安装权限与二进制部署脚本
│   ├── service.sh           # 开机/唤醒启动脚本（调用 chaoxid daemon --bg）
│   ├── uninstall.sh         # 卸载清理脚本
│   ├── bin/
│   │   └── chaoxid          # 静态编译 arm64-musl 守护二进制
│   └── webroot/
│       └── index.html       # 莫奈动态取色 / 自定义壁纸字体交互前端
└── build.ps1                # Zig aarch64-linux-musl 编译与 ZIP 打包脚本
```

---

## 功能与界面

- **独立三簇调控**：自动识别设备 `policy*` CPU 簇（按频段排序），支持 `up_rate_limit_us` 与 `down_rate_limit_us` 联动或独立微调，提供预设胶囊与 Scene 档位一键读取/覆写。
- **莫奈取色与个性化外观**：支持跟随系统 / 浅色 / 深色模式、内置壁纸及自定义壁纸色板提取、背景高斯虚化与卡片透明度调节、圆体 / 楷体 / 等宽字体及本地字体导入。
- **前端仅在开启 WebUI 时运行**：关闭页面不驻留任何 WebView 或前端脚本进程，日常后台仅运行轻量原生 `chaoxid` 守护进程。

---

## 安装与构建

### 直接安装（推荐）
1. 在 [Releases](../../releases) 下载最新 `chaoxi-v1.2.0.zip`（如升级前曾安装旧版兼容模块，建议先禁用或移除避免冲突）。
2. 打开 KernelSU 管理器 → 模块 → 从本地安装 → 刷入 ZIP 并重启（或刷入后点击「WebUI / 操作」配置）。
3. 在「触发策略与日志」页确认模式为 **智能低耗守卫（默认推荐）**；在「速率调校」页选择参数并按需勾选同步覆盖 Scene 档位，点击底部「应用并设为默认」即可。

### 从源码编译（Windows / Zig 工具链）
将 [Zig](https://ziglang.org/) 工具加入 `PATH` 或设置 `$env:ZIG_EXE` 后运行 [build.ps1](build.ps1)：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

将在仓库根目录生成静态链接 `aarch64-linux-musl` 二进制并打包为可刷入 ZIP。

---

## 常用排错命令（Root Shell）

```sh
# 查看运行状态、当前防护模式及各簇 sysfs 与目标值对比
/data/adb/modules/chaoxi/bin/chaoxid status

# 查看事件恢复日志
/data/adb/modules/chaoxi/bin/chaoxid log 60

# 手动应用参数并同步写入 Scene 指定档位 (none|powersave|balance|performance|all)
/data/adb/modules/chaoxi/bin/chaoxid apply 8000 8000 4000 4000 1000 1000 --scheme powersave
```

## 许可

模块源代码遵循 MIT License 开源，可自由修改与二次编译用于个人设备。
