# P7：系统资源查询

**状态：已实现为内置功能（2026-09-06 常驻采集、2026-09-09 系统信息窗口、
2026-09-12 采集时机分段与详情分批预取）；性能收益未用服务端独立工具横向对比，
属功能完成、量化验收待补**

> **范围说明**：本阶段是 SSH 远端资源监控面板与系统信息查询窗口两项内置能力
> 的设计与实现记录，直接编进主程序、直接持有 `QWidget` 与 `SshTransport`。
> 远端 AI 上下文与工具调用能力不在本阶段范围内，规划在下一个大版通过 AI MCP
> （Model Context Protocol）接口引入，届时另立文档。

## 目标

在**不新建第二条 SSH 连接、不阻塞交互 Shell、不反压 Parser** 的前提下，为当前
SSH 会话提供两类远端资源视图：

- **常驻低开销监控**（`SystemMonitorPanel`）：可停靠面板，周期性展示 CPU、
  内存、交换分区占用率，单网卡收发速率历史，以及文件系统容量。
- **分批预取的详细视图**（`SystemInformationDialog`）：面板信息按钮打开的独立
  窗口，展示操作系统、内核、CPU/GPU 硬件、CPU 使用分解、内存/交换、网络接口与
  文件系统的完整明细 —— 详情由连接绑定的预取按批均匀铺开，**不**在连接建立时
  一次性抓取（见「采集时机与分批调度」）。

两者都是**内置实现**，直接持有 `QWidget` 与 `SshTransport`，编进主程序。

## 与当前代码的关系（2026-09-12 核对）

| 能力 | 源文件 |
| --- | --- |
| 常驻监控面板 | `src/ui/widgets/SystemMonitorPanel.{h,cpp}` |
| 系统信息窗口 | `src/ui/widgets/SystemInformationDialog.{h,cpp}` |
| 双通道 SSH 采集 API | `src/transport/SshTransport.h`（`executeCommand` / `startResourceMonitoring` / `requestResourceSample`） |
| 常驻帧协议解析 | `SshMonitorFrameParser`（见 P6「SSH 远端资源监控」小节） |
| 详情分批预取 | `src/service/ResourcePrefetch.h`（绑定 transport）、`src/service/ResourcePrefetchSchedule.h`（分批时序） |
| 远端命令与本地解析 | `src/service/LinuxResourceData.h`（`staticCommand(batch)` / `slowCommand()` / `parseMetrics` / `information`） |
| 面板装载与会话接线 | `src/ui/app/MainWindow.cpp`（dock 装载、会话切换、最小化门控） |

面板由 `MainWindow` 通过 dock 持有；会话切换经
`TerminalPage::currentSftpContextChanged → SystemMonitorPanel::setSessionContext`
传入具体 `SshTransport`，主窗口最小化状态经 `setPresentationActive` 传入。

## 数据流

```mermaid
flowchart LR
    subgraph GUI线程
        MP[SystemMonitorPanel]
        SID[SystemInformationDialog]
        PF[ResourcePrefetch<br/>绑定 transport]
    end
    subgraph SSH工作线程
        RC[常驻 exec channel<br/>/proc/stat+meminfo+net/dev]
        OC[单次 exec channel<br/>df-only / 分批静态命令]
    end
    MP -- requestResourceSample --> RC
    RC -- resourceSampleFinished --> MP
    MP -- executeCommand: df-only --> OC
    PF -- executeCommand: staticCommand batch --> OC
    OC -- commandFinished --> PF
    PF -- data: 分批累积 --> MP
    MP -- populate(output, pending) --> SID
    RC -. 复用同一 SSH 连接 .- OC
```

关键点：**快速 `/proc` 指标与慢 `df` 走完全隔离的两条 channel**。慢挂载点或
繁忙服务端拖住 `df` 时，CPU/内存/网络刷新不受影响。

系统信息窗口**不自己发远端命令**：详情由 `ResourcePrefetch` 分批取回，面板每秒
把「预取累积的静态段 + `df` 结果 + 最新样本」在本地合成为信息行（
`LinuxResource::information()`，全部计算在客户端），再推给窗口渲染；窗口只做
展示与本地解析（`parseInformation()`）。

单次命令通道是**有界、单请求**的，因此 `df` 与预取批次互斥：面板用
`ResourcePrefetch::allowsSlowQuery()` 错峰（只允许首帧前先行），被占用时按
`FileSystemDeferralMs` 退避重试，不并发创建第二条远端命令。

## 命令与稳定协议

所有远端脚本都用「类型标记 + 制表符字段」的稳定协议，绝不依赖本地化输出
文本，脚本开头统一 `LC_ALL=C`。

### 常驻快速采集（每 2 秒，可配 1 秒）

常驻脚本由 `SshTransport` 在无 PTY 的常驻 channel 内维护，每次请求只启动一个
合并读取 `/proc/stat`、`/proc/meminfo`、`/proc/net/dev` 的 `awk`。面板侧解析器
`parseMetrics()`（`SystemMonitorPanel.cpp:178`）约定：

| 类型 | 字段 | 含义 |
| --- | --- | --- |
| `CPU` | 3 | `CPU\t<total tick>\t<idle tick>` |
| `MEM` | 5 | `MEM\t<memTotalKiB>\t<memAvailKiB>\t<swapTotalKiB>\t<swapFreeKiB>` |
| `NET` | 4 | `NET\t<iface>\t<recvBytes>\t<sentBytes>` |

`CPU` 与 `MEM` 缺任一即视为本次采集无效；`NET` 上限 128 个接口。

### 低频文件系统（每 10 秒，仅 df）

`slowCommand(/*includeFrequency=*/false)`（`src/service/LinuxResourceData.h`）：
在 `@@filesystems` 分节内输出 `df -Pk || df -k` 原文，由
`LinuxResource::fileSystems()` 解析出 `device / size / used% / avail / mount`，
上限 128 个文件系统（面板侧 `parseFileSystems()` 只取容量两列填磁盘列表）。
CPU 频率不在这里读 —— 它随详情批次 1 的 `cpuinfo` 一起取，避免同一字段两处重复
查询。

### 系统信息分批命令（3 批，连接后均匀铺开）

详情不再是"一条大命令"，而是 `staticCommand(batch)` 的 3 条小命令，由
`ResourcePrefetch` 按 `PrefetchSchedule` 分批提交（见下一节）。
**批次数即远端命令数，是远端 CPU 成本的主要来源**：实测（见下节）每条命令
约 15ms CPU 且与内容基本无关，因此概览与 `ip` 合并、`cpuinfo` 独立一批
（超大主机上输出超限时不会连带丢掉概览）、`lspci` 单独放最后：

| 批次 | 远端命令要点 | 覆盖分节 |
| --- | --- | --- |
| 0 | `/etc/os-release`、`/proc/sys/kernel/{osrelease,hostname}`、`uname -m`、`$SSH_CONNECTION`、`ip -o -4 addr show scope global` | `@@os` `@@kernel` `@@host` `@@arch` `@@connection` `@@ip` |
| 1 | `/proc/cpuinfo` + `scaling_cur_freq`（缺失则回落 `cpu MHz`） | `@@cpuinfo` `@@frequency` |
| 2 | `lspci` | `@@gpu` |

每批以小节的 `@@done` 收尾，`ResourcePrefetch` 只把带 `@@done` 的输出计入累积
数据；缺命令、超时或权限不足只影响该批字段，不覆盖已取得的其他分节。

面板把「预取累积的静态段 + `df` 结果 + 最新样本」在本地合成为信息行
（`LinuxResource::information()`），类型与卡片对应关系：

| 类型 | 卡片 | 关键字段 |
| --- | --- | --- |
| `OV` | Overview | os / kernel / host / ip / load / arch / uptime / SSH_CONNECTION |
| `CPU` | CPU | name / cores / MHz / cache / vendor / BogoMIPS |
| `GPU` | GPU | `lspci` 匹配 VGA/3D/Display 控制器 |
| `CPUUSE` | CPU usage | user / system / nice / idle / iowait / irq+softirq+steal 百分比 |
| `MEM` `SWAP` | Memory / Swap | total / used / avail(free) / usage% / cache |
| `NET` | Network interfaces | iface / sent / received |
| `FS` | Filesystems | device / size / used% / avail / mount |

`parseInformation()`（`SystemInformationDialog.cpp`）按首字段分派，容量统一以
1024 进位换算为可读单位（`formatKiB`/`formatBytes`），uptime 秒数格式化为
`d/h/min`。

## 采集时机与分批调度

连接建立后的采集分两段，避免握手瞬间在服务端堆起多条命令抬高 CPU，同时保证
用户点开详情时已有内容：

| 时段 | 采集内容 | 归属 |
| --- | --- | --- |
| 连接即开始 | 常驻通道的 CPU/内存/交换/网络（每秒） | 面板 |
| 连接 +1.2s | 首次 `df`（之后每 10 秒，仅文件系统） | 面板磁盘列表 |
| 连接 +1.2s 起，每批间隔 1.0s | 详情 3 批（概览+ip → cpuinfo+频率 → lspci） | 系统信息窗口 |

调度常量与不变量集中在 `src/service/ResourcePrefetchSchedule.h`：

- **首批 1.2s**：概览（`@@os`/`@@kernel`/`@@host`/`@@arch`/`@@connection`/`@@ip`）
  最先就绪 —— 连接成功 **2~3 秒**后点 `_infoButton` 已有内容；
- **批间隔 1.0s、总时长 ≤3.5s、批次数 ≤3**：最重的 `lspci` 排最后一批，慢批次
  结束后还要吃满完成冷却（250ms），不立刻补发；
- 详情预取**绑定 transport**（`ResourcePrefetch::forTransport`，父对象是
  transport）：切换标签不重启采集、已取到的分节继续复用；回连按
  `connectionGeneration()` 换代重来，不跨连接复用。

对话框按"已有分节"渲染，未到的卡片显示「采集中」而非「No data」
（`SystemInformationDialog::populate(output, pending)`，`pending` 取自
`ResourcePrefetch::complete()`），因此首批到达即可展示，后续批次在面板下一次
每秒刷新时补齐。

### 实机测量（2026-09-12，root@192.168.10.100）

设备为 **2 核 ARMv7**（`grep -c '^cpu[0-9]' /proc/stat` = 2，内核自报
`ARMv7 Processor rev 0 (v7l)`）。用 `novaterm_ssh_monitor_integration_check`
的两个实测模式取得数据（都用「重复执行 + `/proc/stat` 前后差」，不依赖远端
`time`/`times` 的可用性）：

- `--profile-commands`：逐命令成本。**每条远端命令约 15ms CPU，且与命令内容
  基本无关** —— 固定开销来自远端 shell 与通道处理；因此减少命令数（4 批→3 批、
  `df` 延迟）比拉开间隔更有效。
- `--profile-collection [--no-baseline] [--defer-df=] [--batch-start=] [--batch-gap=]`：
  逐秒打印整机 CPU 占用率（与面板同口径），可对比不同策略。

实测对比（同时刻、同一设备，`--no-baseline` 复现"连接即采集"）：

| 计划 | 首个区间(0–1s) | 峰值 |
| --- | --- | --- |
| 旧：`df`@0ms、批次 0.4/1.2/2.0s | **4.0%** | 4.0% |
| 新：`df`@1.2s、批次 1.2/2.2/3.2s | **1.0%** | 3.0% |

据此面板把**连接后前两个采样区间作为预热**（`WarmupIntervals=2`）：登录 shell
启动、常驻通道建立、首帧 `df` 与概览批都落在其中，只更新差分基线、不发布
CPU/网络读数，避免把一次性启动开销显示成远端稳态占用。

### 读数的来源（排查"CPU 偏高"前必读）

面板显示的 **CPU 占用率与网速都是本地按"相邻两帧差值"算出来的**，远端只回原始
文本：常驻脚本输出 `/proc/stat` 首行、`/proc/meminfo` 与 `/proc/net/dev` 原文、
`/proc/loadavg`、`/proc/uptime`（`SshTransport.cpp` 的 `resourceMonitorCommand()`）。
`LinuxResource::parseMetrics()`/`cpuUsage()` 与
`SystemMonitorPanel::handleFastMetrics()` 负责解析与差分；`cpuPercent` 用聚合
`cpu` 行的 `(total-idle)/total`，网速用字节差 ÷ 实测间隔。**因此采样区间里我方
命令的开销会被计入读数**（该设备上单条命令约占 1~2 个百分点），分辨"远端真实
负载 / 我方采集开销 / 预热区间"是判断读数是否可信的前提。

## 采样生命周期与门控

`updateSamplingState()`（`SystemMonitorPanel.cpp`）是采样开关的唯一判据：

```
shouldSample = _presentationActive
            && (isVisible() || 系统信息窗口可见)
            && _sshTransport && _sshTransport->isConnected()
```

- **隐藏、折叠、最小化、切换标签** → 停止两个定时器、`stopResourceMonitoring()`
  发 EOF 关闭常驻 channel、取消在途 `df`，并**清空 CPU/网络差分基线**
  （`_previousCpuTotal`、`_previousNetworkBytes` 等）。
- **恢复** → 重启定时器、`startResourceMonitoring()`、立即各触发一次采样，
  重新建立基线。
- **门控只管面板自己的两条通道**：详情分批预取绑定 transport、不受面板可见性
  影响 —— 面板隐藏期间它照常按批走完（总量有界：4 条小命令），这样切走标签再
  回来、或稍后打开详情窗口时不必重头采集。

暂停期间远端累计计数（`/proc/stat` tick、`/proc/net/dev` 字节）持续变化，
恢复时**必须丢弃旧基线**，否则首个样本会算出跨越整个暂停期的虚高速率/占用。

## 差分与数值安全

- **CPU 占用**：`/proc/stat` 是开机以来累计 tick，首样本仅建基线，之后用相邻
  样本的 `(total-idle)/total` 差分（`handleFastMetrics` 内）。
- **网络速率**：用相邻样本字节差 ÷ **真实采样间隔**（`QElapsedTimer` 实测，
  非固定 1 秒），兼容定时器抖动与命令执行耗时；计数器回绕或网卡重置时差值
  钳为 0，避免尖峰。历史保留最近 `NetworkHistoryCapacity = 64` 个点。
- **占用率百分比**：`used * 100` 先提升到 `long double` 再除，避免大容量主机
  整数溢出；结果 `clamp(0,100)`。
- **交换分区总量为 0**：合法（未配置 swap），显示真实 `0%` 而非「未知」。

## 会话切换与迟到结果屏蔽

`setSessionContext()`（`SystemMonitorPanel.cpp:525`）切换 transport 时：捕获
当前 `SshTransport*`，所有异步回调（`commandFinished` / `resourceSampleFinished`
/ `disconnected` / `destroyed`）在 lambda 内复核 `_sshTransport == current`，
屏蔽切换会话后迟到的旧结果；`resetMetrics()` 同步清空全部累计基线与 pending
请求 ID（`pending == 0` 表示无在途请求）。系统信息窗口另以请求 ID +
`QPointer<SshTransport>` + `WA_DeleteOnClose` 三重屏蔽迟到结果。

面板与窗口都监听 `LanguageManager::languageChanged` 与 `ElaTheme::themeModeChanged`
做运行时语言/主题切换；`SystemMonitorPanel::paintEvent` 内比对
`_themeMode` 做主题自愈（与 `ElaText.cpp:158` 同理，不把配色只押在信号按期
到达上）。

## UI 测试覆盖与缺口

`src/ui/` 基本无覆盖测试（见 AGENTS.md「改哪测哪」表），本阶段的可验证部分
主要落在 Transport 层与调度层：

- `novaterm_ssh_transport_check`：常驻帧协议分片/合帧/错误帧、单帧与接收
  缓冲与条目上限、未连接拒绝采样、`SSH_EOF`/`SSH_ERROR` 区分、EOF 与
  exit-status 到达顺序；**采集时机**断言按 `PrefetchSchedule` 常量推导
  （首批 ≤1.5s、批间隔 ≥500ms、总时长 ≤3.5s、批次数 ≤3、慢批次不补发），
  并校验分批次序为「概览（含 ip）最先、`cpuinfo` 独立一批、`lspci` 最后、
  每批 `@@done` 收尾、无多余批次」。
- `novaterm_ssh_monitor_integration_check`（不注册 ctest，需真实服务端）：
  慢命令并发、交互 I/O、暂停回收、断开重连、迟到样本为 0；另有
  `--profile-commands` / `--profile-collection` 两个实测模式用于采集时机与
  逐命令成本量化（见「实机测量」）。2026-09-12 实测 root@192.168.10.100
  通过：`samples=10 during_slow=5 slow_timeout=1 terminal_io=1
  process_gone=1 reconnect=1 late=0`。

- `novaterm_ui_dialog_layout_tests`（`tests/ui/`，offscreen，<1s）：系统信息
  对话框的滚动范围回归。构造接近真实的 TSV 载荷后按 1100x760 / 700x420 /
  1100x900 三种尺寸断言「内容高度 == max(视口高, `heightForWidth(视口宽)`)」
  与「滚到底内容底边贴视口底（±8px）」。用于锁住
  `SystemInformationDialog::updateContentHeight()` 的取值方式 —— 内容高度若
  交回布局的 `minimumSizeHint()`，含 `setWordWrap(true)` 的布局会被高估
  （实测 886px 的内容给到 989px），滚到底会多出一页空白。

面板与窗口本身的其余布局、门控、差分逻辑靠编译通过 + 实跑验证。

## 实施边界（硬约束）

- **不新建第二条 SSH 连接**：所有采集复用当前会话的 `SshTransport`，通过其
  辅助 exec channel。
- **快慢通道隔离**：`/proc` 快采与 `df` 分属两条 channel，慢 `df` 不得拖住
  快指标。
- **最多一个在途请求**：`requestFastMetrics` / `requestFileSystems` 非重入，
  慢服务端不积压定时任务。
- **不可见即停采**：任何不可见状态都必须停止远端脚本并释放 channel，恢复时
  重建基线。
- **不反压 Parser / 交互 Shell**：辅助 channel 与交互 Shell 共享同一 SSH 工作
  线程，全程非阻塞状态机处理 `SSH_AGAIN`，资源采集不得阻塞交互回显。

## 剩余工作

- 尚未用服务端独立观测工具（整机 CPU / 进程树 CPU 时间）对原始每秒单命令
  实现与当前常驻方案做量化对比，因此不能声明具体性能收益数字（见 P6
  「SSH 远端资源监控」小节同一保留）。分批时序本身已有断言与真机连通性验收，
  但"远端 CPU 峰值是否下降"仍未量化。
- 仅覆盖 Linux `/proc` 与 `df`；非 Linux 远端返回空指标并显示明确空状态，
  未提供其它平台的采集脚本。
- 面板与窗口本身无自动化回归；目前只有系统信息对话框的滚动范围有
  `novaterm_ui_dialog_layout_tests` 覆盖，卡片内容、门控与差分逻辑仍依赖人工实跑。

## 相关文档

- P6「SSH 远端资源监控（2026-09-06 起）」：常驻通道、帧协议、超时退避、
  `SSH_EOF` 与 exit-status 竞态的实现明细与实测记录。
- `docs/ARCHITECTURE.md`：资源面板的标题发布与最小宽度策略、信息窗口定位。
- `docs/architecture/Performance_Optimization_2026-09-10.md`：SSH worker 唤醒
  与有界交付的同批优化。
