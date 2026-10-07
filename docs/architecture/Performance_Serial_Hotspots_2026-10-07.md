# Windows 串口 CPU 热点采样（2026-10-07）

## 结论与投入建议

Windows 采样已完成，当前无需因工具问题转到 Linux。
本机使用 WPR 录制 CPU 样本与调用栈，xperf 加载本地 PDB，成功解析了第一方
Core、libvterm 和 Renderer 函数。可见／隐藏两项 8M、64 MiB 数据均完整收到。
工具用途与 CPU Usage (Sampled) 方法见
[Microsoft WPR/WPA 说明](https://learn.microsoft.com/en-us/troubleshoot/windows-server/support-tools/support-tools-xperf-wpa-wpr)。

**优化紧迫性不高，适合优先做一项小范围 A/B，收益潜力中等。**
扩大驱动队列后的接收完整性和线速已满足本轮要求；热点采样进一步把优先级
收敛到 **VT 屏幕模型同步和小输入批次**。这是跨可见／隐藏模式都存在的成本，
值得先检查重复同步频率与 Cell 转换，随后再判断具体改动收益。
本轮没有实施生产优化，也没有把理论收益当作实测收益。

## 采样条件与质量

- Windows 26200，i7-14700K，28 个逻辑处理器，Qt 6.8.3。
- 独立优化构建：Release `/O2 /Ob2`，Core 和 libvterm 从当前源码重建，
  `/Z7` 与 `/DEBUG:FULL` 提供 PDB；没有改成 Debug 或开启函数插桩。
- 与大队列复测相同：COM1 → COM2，8M、8N1、无流控、64 MiB，稳定计时，
  延后帧校验；每次核对驱动 RX 队列实际为 262144 字节。
- 程序先生成数据集并初始化窗口，再通过控制文件握手开始 WPR；数据阶段
  结束后停止 WPR，随后才做离线 CRC／序号校验。ETL 约 104.8 s，其中约
  83.9 s 为发送阶段，其余包含开始／停止控制与 rundown 等空闲等待。
- 两项均为 524288 / 524288 帧，零缺失、损坏、重复、乱序，SHA-256 一致；
  两份 ETL 的 Lost Buffers / Lost Events 均为 0。
- WPR 会话已停止；原始 ETL 仅保留在本机 gitignore 的 build 目录。
- 采样期间进程 CPU 为可见 76.39%、隐藏 73.42%，均按一个逻辑核为 100%。
  绝对值高于先前未录制场景，存在采样扰动／场景波动，不能直接当作生产
  CPU 基线或改动前后对比；本轮采用函数和线程内部相对占比排序。

本地没有 Qt、Windows 内核和虚拟驱动的完整 PDB，部分外部函数仅显示
`Unknown`。第一方 PDB 可用；这些外部未知项不妨碍确认下面的第一方热点，
但限制了对系统／Qt 内部成本的进一步归因。

## 线程分离

线程 ID 由程序记录，Parser 通过 `nvterm-parser` 名称识别。
调用栈只筛选 CPU Profile 事件，不把上下文切换／等待栈混作 CPU 热点。

| 模式 | 进程 PID | GUI TID / CPU 栈样本 | Parser TID / CPU 栈样本 | 发送夹具 TID / CPU 栈样本 |
| --- | --- | --- | --- | --- |
| 可见 | 12456 | 22756 / 22951 | 32644 / 12853 | 23192 / 10300 |
| 隐藏 | 19428 | 12772 / 12593 | 21908 / 14303 | 37272 / 10211 |

发送夹具单列分析；它的内核／虚拟驱动工作不被当作 Parser 或 GUI 热点。
上述样本数为线程 CPU 栈命中数，不是调用次数或墙钟耗时。
函数进程报告使用 xperf 加权样本，线程报告使用栈命中比例，两者分母不同。

## 首要热点：屏幕同步链

以下为 **Parser 线程内 exclusive 栈样本占比**，三项互不重叠，可以相加。

| 函数 | 可见模式 | 隐藏模式 | 源码位置 |
| --- | --- | --- | --- |
| `vterm_screen_get_cells` | 27.82% | 29.85% | `third_party/libvterm-0.3.3/src/screen.c:1075` |
| `VTAdapter::Impl::syncRegion` | 23.93% | 25.34% | `src/core/terminal/VTAdapter.cpp:253` |
| `populateCell` | 15.01% | 15.26% | `src/core/terminal/VTAdapter.cpp:124` |
| **合计** | **66.76%** | **70.45%** | 按行读取 libvterm → 转换 Cell → 写入模型 |

在包含发送器、GUI、Parser 和其他工作线程的全进程加权统计中，三者合计为
**17.26%（可见）／24.45%（隐藏）**。
`syncRegion` 的 inclusive 样本包含其子调用，因此不能再与子调用的 inclusive
百分比相加；上表使用 exclusive 值，避免重复记账。

同一输入模式在隐藏窗口仍有同样的主要同步成本。结合源码每次取到输入后
立即解析、flushDamage、同步模型、发布信号的路径，小批次输入造成反复同步
是优先验证的候选原因；采样确认了热点，尚未证明某个批次策略一定能取得收益。

优化时必须保留 libvterm 边界、Parser 单写、快照一致性、控制命令字节屏障和
waitForIdle 语义；不能跳过必要同步或借用尚未更新的旧 Cell 来换取基准分数。

## GUI 路径与优先级

以下是 GUI 线程 **inclusive** 栈样本占比，只描述调用子树，不能逐项相加：

| 调用子树 | 可见 GUI | 隐藏 GUI |
| --- | --- | --- |
| `TerminalRenderer::render` | 42.39% | 无实际 GPU 帧 |
| `rebuildCommandRows` | 24.43% | — |
| `appendCellCommands` | 23.55% | — |
| `ensureGlyph` | 15.18% | — |
| `uploadCommands` | 9.41% | — |
| `SerialTransport::readAvailable` | 5.45% | 11.11% |
| `updateHistoryLayout` | 属于其他子树，未列入此表 | 8.35% |

可见场景的命令生成／glyph 工作仍值得后续检查，但串口优化可先从两种模式
共有的 VT 同步链入手。隐藏布局维护有成本，采样没有支持将它作为本轮最主要
的全进程热点。外部 Qt／系统符号不足，不能将所有 `Unknown` 归给本项目。

## 收益如何估计

若仅作理论估算，假设上述同步链耗时能减半，按全进程样本占比可推算 CPU
时间下降约 **8.6%（可见）／12.2%（隐藏）**。这是条件推算，不是已实现收益。
真实主程序没有测试发送器，分母和采样扰动也会变化，需要不录制时的同机 A/B
确认。收益主要体现为更低 CPU 和更大的响应余量；本轮吞吐受 8M 发送速率约束。

建议下一步只验证一个小范围方案，例如减少小输入批次造成的重复同步，或
缩短同步链中的重复 Cell 字段转换，保留完整回归与差分验证。若同机 A/B 收益
落在噪声内，就保留现状，不开展全面性能重构。
Linux `perf` 可作为跨平台交叉复核，当前 Windows 已取得可用函数级证据。

## 可复测的产物

- [可见全进程函数 CSV](serial-hotspots-2026-10-07-visible.csv)
- [隐藏全进程函数 CSV](serial-hotspots-2026-10-07-hidden.csv)
- [线程样本汇总](serial-hotspots-2026-10-07-threads.csv)
- 采集脚本：`tests/benchmarks/serial-stress/profile-windows.ps1`
- 解析脚本：`tests/benchmarks/serial-stress/analyze-windows-profile.py`

采集使用已验证的 PowerShell 7。脚本只停止自己启动的 WPR，会拒绝覆盖其他
正在进行的录制。完整 ETL、线程 butterfly HTML、符号缓存和带 PDB 的二进制
在 `build/serial-stress-profile/` 与 `build/serial-stress-symbols/`，不提交到仓库。
没有修改生产编译选项或安装新采样工具。
