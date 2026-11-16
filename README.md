# 潮汐 · Tide（CPU Rate Limit 事件守护模块）

一款基于 **Linux 内核事件机制（`epoll` + `inotify` + `Netlink` + `timerfd`）** 与 **静态 C++17** 实现的 KernelSU 模块，专注解决多集群 CPU `up_rate_limit_us` / `down_rate_limit_us` 参数在**拔掉充电器、低电量、长时间息屏再亮屏及调频器切换**后被系统或电源管理服务覆盖回默认值的问题。

模块内置 **莫奈动态取色 WebUI**，支持自定义壁纸与字体，并支持将配置**直接设为系统与模块默认参数，同时统一覆写 Scene 调度策略档位**，在不干预常规调频与日常触控提频的前提下恢复正常待机与亮屏功耗。

---

## 核心调控逻辑（v1.1.0 改进架构）

针对传统“高频轮询脚本 / 全时段抢占节点”造成额外唤醒与功耗升高的问题，**潮汐** 采用**低感知、场景触发、写读分离**的事件驱动设计：

1. **全事件零轮询架构（`chaoxid`）**
   - 主循环完全阻塞在内核 `epoll_wait(..., -1)` 上等待中断事件，日常空闲期不产生定时器轮询，静态编译体积轻巧、常驻内存百 KB 级。
   - 仅定位并写入各集群调频器目录（如 `sugov_ext` / `schedutil`）下的 `up_rate_limit_us` 与 `down_rate_limit_us`，**绝不修改 `scaling_governor` 或 `scaling_min_freq` / `scaling_max_freq`**。

2. **三档覆盖防护模式（默认：`passive` 宽松被动）**
   - **`passive`（宽松被动 · 省电推荐）**
     - **零持续节点监听**：完全撤除 `up_rate_limit_us` / `down_rate_limit_us` 的 `inotify` 盯防，绝不与系统正常的短时触控升频（例如短暂切换 `0/6000`）对抗，彻底消除日常滑动时的无效上下文切换与锁频竞争。
     - **高风险场景被动恢复**：仅响应内核发来的关键硬件/状态变化：
       - **拔充恢复**：通过 Netlink `KOBJECT_UEVENT` 原始缓冲预过滤 `SUBSYSTEM=power_supply`，对多电源节点突发事件做 `2000ms` 去重合并，仅在拔充后执行轻量化核对。
       - **低电量保护**：跨越低电量阈值或每下降 5% 时触发一次被动校验。
       - **长息屏再亮屏校准**：监听背光节点 `0 -> >0` 唤醒，息屏时长 `>= 2s` 时延迟 `1300ms` 执行单次校准（避开亮屏解锁瞬间的系统提频，检测到活跃提频时自动避让）。
       - **调频器目录重建**：监听策略目录 `IN_CREATE` 与 `scaling_governor` 真实切换（例如从性能模式切回时重建 rate 节点），自动重新挂载监听并恢复参数。
     - **亮度渐变防抖与低闪存写入**：当屏幕处于点亮状态下触发自动亮度平滑渐变时，立即临时摘除背光监听 `1800–2000ms` 后静默重挂，消除密集亮度调节唤醒；状态计数文件节流至 `300s`（5 分钟）或退出时落盘，常规状态下仅在发生实际修复时写日志。
   - **`scene`（Scene 策略联动）**
     - 保持日常零主动争夺逻辑，额外监听 Scene `profile.json` 切换（自动排除自身写入后 `3000ms` 自触发窗口）。
   - **`lock`（强力实时锁定）**
     - 实时监控节点改写并在被覆写时写回；内置 `2000ms` 内高频争抢超 4 次自动进入 **`10s` 冷却退避** 的保护熔断。

3. **系统 · 模块默认 · Scene 策略统一写入**
   - 前端点击写入或通过 `chaoxid apply ... --scheme <none|powersave|balance|performance|all>` 执行时：
     1. 立即原子写入内核 `sysfs` 并刷新守护内存目标；
     2. 同步持久化为 `/data/adb/chaoxi/config.conf` 模块与系统默认配置（移除旧版自动回读覆写逻辑，确保手动修改不再被旧配置顶回）；
     3. 如指定 Scene 档位，同步保留原属主与权限原子修改 Scene `profile.json` 对应策略块及 `objects/<scheme>` 序列化对象，使**系统当前值、模块默认值与 Scene 策略值三方保持一致**。

---

## 仓库目录结构

```text
.
├── src/
│   └── chaoxid.cpp          # C++17 核心事件守护进程与 CLI / Scene 写穿引擎
├── module/
│   ├── module.prop          # KernelSU 模块元数据 (id=chaoxi)
│   ├── customize.sh         # 安装权限与二进制部署脚本
│   ├── service.sh           # 开机自启守护脚本
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
1. 在 [Releases](../../releases) 下载最新 `chaoxi-v1.1.0.zip`（如升级前曾安装旧测试版或同名兼容模块，建议先禁用或清理旧版避免冲突）。
2. 打开 KernelSU 管理器 → 模块 → 从本地安装 → 刷入 ZIP 并重启，或刷入后在模块列表点击「WebUI / 操作」进入配置界面。
3. 在「触发策略与日志」页确认模式为 **宽松被动（省电推荐）**；在「速率调校」页选择参数并按需勾选同步覆盖 Scene 档位，点击底部「应用并设为默认」即可。

### 从源码编译（Windows / Zig 工具链）
需安装 [Zig](https://ziglang.org/) 工具链并调整 [build.ps1](build.ps1) 中的路径变量后运行：

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
