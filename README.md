# 潮汐 · Tide（CPU Rate Limit 内核护盾与事件守护模块）

一款基于 **Linux VFS 权限隔离（`0444` 零唤醒护盾）**、**内核事件机制（`epoll` + `inotify` + `Netlink` + `timerfd`）** 与 **静态 C++17** 实现的 KernelSU 模块，专注解决多集群 CPU `up_rate_limit_us` / `down_rate_limit_us` 参数在日常提频结束、拔插充电器、低电量、长时间息屏再亮屏及调频器切换后被系统 PowerHAL 重置回 `10us` 等默认值，以及历史配置反复回写覆盖最新设定的问题。

模块内置 **莫奈动态取色 WebUI**，默认支持 **Up / Down 独立非对称微调**（例如高升频延迟 + 低降频延迟省电策略）、自定义壁纸与字体，并支持将配置**直接设为系统与模块默认参数，同时按需同步覆写 Scene 调度策略档位**。

---

## 核心调控逻辑（v1.3.0 架构）

传统轮询或纯 `inotify` 事后修补方案每次都要等系统 PowerHAL 把节点改写为 `0us`/`10us` 之后再唤醒守护进程抢写，不仅高频产生上下文切换与日志 I/O，还会导致非对称 `up > down` 设定刚写入就被改乱。**潮汐 v1.3.0** 从内核权限层与调度器底层重构为 **「内核层 VFS `0444` 权限护盾 + 清晰三档分离 + 独立非对称直写」**：

1. **内核 VFS `0444` 权限隔离原理（源头零唤醒）**
   - 系统电源 HAL 服务通常以无 `CAP_DAC_OVERRIDE` 的受限能力运行；当 `chaoxid` 将各集群调频器目录（`sugov_ext` / `schedutil`）下的 `up_rate_limit_us` 与 `down_rate_limit_us` 节点设为 `0444`（只读）后，PowerHAL 的 `10us` / `100us` / `0us` 改写会在内核 VFS 层被直接拒绝（`EACCES`），**不崩溃、不触发 SELinux 拒绝、不影响正常频率上下限提频，且守护进程日常唤醒次数降至 0**。
   - 同时具备 `CAP_DAC_OVERRIDE` 的高权限工具（如 `chaoxid` 自身与 `scene-daemon`）仍可无障碍写入；当某集群从 `performance` 切回 `sugov_ext` 导致内核以 `0644` 重建节点时，`chaoxid` 的目录与调频器监听会在毫秒内自动重新加锁 `0444` 并校准参数。
   - 当用户暂停守护、切到兼容提频模式、停止进程或卸载模块时，自动还原节点权限为 `0664`，不留任何残留锁死。

2. **三档明确定位（彻底消除前两档重叠与旧参数篡改）**
   - **`override`（潮汐独占 · 全局零唤醒硬锁 · 默认推荐）**
     - 启用内核 `0444` 护盾从源头屏蔽 PowerHAL 改写（日常 `0` 唤醒）；
     - **全局唯一以潮汐最新设定为准**，彻底移除自动吸附旧档位逻辑；即便外部高权限调度工具因切应用写入未同步的历史参数，也会在 `80ms` 内自动弹回最新目标值，杜绝旧参数反复复活篡改。
   - **`shield`（Scene 护盾 · 分档零唤醒）**
     - 同样启用内核 `0444` 彻底屏蔽系统 `10us` 重置，**关闭数值写入监听（仅监听 `IN_ATTRIB` 权限变动，日常操作 0 唤醒极省电）**；
     - 利用权限分离特性完全放行 Scene 按应用自由切换 `powersave` / `balance` / `performance` 不同档位，若 Scene 切换档位时临时放宽权限至 `0664`，守护进程在 `<1ms` 内自动重锁回 `0444` 且绝不覆盖 Scene 写入的有效数值。
   - **`compat`（兼容提频 · 软守护）**
     - 还原节点权限为 `0664`，专门给希望保留系统短时 `0us` 触控/启动提频的用户；
     - 采用 `220ms` 批合并、`80ms` 自写入屏蔽、`up == 0` 提频放行与高频风暴退避，仅在系统提频结束跌回 `10us` 时静默写回潮汐目标值。

3. **根 cgroup 防掉线与全链路低功耗**
   - 无论是开机脚本还是 WebUI 拉起，`chaoxid --bg` 均执行双重 `fork`、`setsid()`、stdio 隔离，逃逸出应用受限 memcg/top-app cgroup 至根级与 `/sys/fs/cgroup/uid_0/pid_*`，并设置 `oom_score_adj = -900`，长驻稳定不掉线，内存占用 ~`300 KB`。
   - 仅操作 `up_rate_limit_us` 与 `down_rate_limit_us`，**绝不修改 `scaling_governor` 或频点上下限**；亮屏自动亮度防抖退避至 `8s`、状态落盘节流 `300s`、高频日志自动折叠，最大程度压低待机与亮屏开销。

4. **系统 · 模块默认 · Scene 策略统一写入**
   - 支持 `chaoxid apply <u0 d0 u1 d1 ...> --scheme <none|powersave|balance|performance|all>`：
     1. 原生支持任意非对称组合（如高 `Up` 缓升 + 低 `Down` 快降），直写内核并即时回读校验；
     2. 持久化为 `/data/adb/chaoxi/config.conf` 默认目标并同步重锁护盾；
     3. 如指定 Scene 档位，保留原属主与权限原子覆写 `profile.json` 对应策略块及 `objects/<scheme>` 对象，让系统、潮汐默认与 Scene 策略完全一致。

---

## 仓库目录结构

```text
.
├── src/
│   └── chaoxid.cpp          # C++17 核心守护进程、0444 VFS 护盾、cgroup 防护与 Scene 写穿引擎
├── module/
│   ├── module.prop          # KernelSU 模块元数据 (id=chaoxi)
│   ├── customize.sh         # 安装权限与二进制部署脚本
│   ├── service.sh           # 开机/唤醒启动脚本（调用 chaoxid daemon --bg）
│   ├── uninstall.sh         # 卸载恢复 0664 权限与数据清理脚本
│   ├── bin/
│   │   └── chaoxid          # 静态编译 arm64-musl 守护二进制
│   └── webroot/
│       └── index.html       # 莫奈动态取色 / 独立非对称调校 / 自定义壁纸字体前端
└── build.ps1                # Zig aarch64-linux-musl 编译与 ZIP 打包脚本
```

---

## 功能与界面

- **独立非对称多簇调控**：自动识别设备 `policy*` CPU 簇并默认开启 **Up / Down 独立调节**，支持直接数字输入、`±250us` 微调、`100us` 精细滑块以及「缓升快降 · 极省」「均衡缓升」等一键非对称预设；页面实时展示各簇 `0444 护盾` 状态与 Scene 各档现状。
- **莫奈取色与个性化外观**：支持跟随系统 / 浅色 / 深色模式、内置壁纸及自定义壁纸色板提取、背景高斯虚化与卡片透明度调节、圆体 / 衬线 / 楷体 / 等宽字体及本地字体导入。
- **零常驻前端**：WebUI 仅在主动打开时渲染，关闭后不驻留任何前端或 WebView 进程。

---

## 安装与构建

### 直接安装（推荐）
1. 在 [Releases](../../releases) 下载最新 `chaoxi-v1.3.0.zip`（如升级前曾安装旧版兼容模块，建议先禁用或移除避免冲突）。
2. 打开 KernelSU 管理器 → 模块 → 从本地安装 → 刷入 ZIP 并重启（或刷入后打开 WebUI 配置）。
3. 在「触发策略与日志」页按需选择 **潮汐独占（全局硬锁，默认推荐）** 或 **Scene 护盾（分档零唤醒）**；在「速率调校」页设置独立的 Up / Down 参数，按需勾选同步覆盖 Scene 档位后点击底部按钮即可立即生效。

### 从源码编译（Windows / Zig 工具链）
将 [Zig](https://ziglang.org/) 工具加入 `PATH` 或设置 `$env:ZIG_EXE` 后运行 [build.ps1](build.ps1)：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

将在仓库根目录生成静态链接 `aarch64-linux-musl` 二进制并打包为可刷入 ZIP。

---

## 常用排错命令（Root Shell）

```sh
# 查看运行状态、0444 护盾状态、当前模式及各簇 sysfs 与目标值对比
/data/adb/modules/chaoxi/bin/chaoxid status

# 查看事件恢复日志
/data/adb/modules/chaoxi/bin/chaoxid log 60

# 手动应用非对称参数并同步写入 Scene 全部 3 档 (none|powersave|balance|performance|all)
/data/adb/modules/chaoxi/bin/chaoxid apply 16000 2000 8000 1500 2000 500 --scheme all
```

## 许可

模块源代码遵循 MIT License 开源，可自由修改与二次编译用于个人设备。
