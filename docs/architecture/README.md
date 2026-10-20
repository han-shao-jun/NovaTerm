# NovaTerm 架构文档索引

> 文档集版本：2.1  
> 整理日期：2026-09-02（本表状态以各阶段文档自身的状态行为准，已同步）  
> 适用项目版本：NovaTerm 0.1.x 及后续重构阶段

本目录收录 NovaTerm 的架构配套文档。统一架构总览位于 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md)；本目录中的配置、渲染、路线图和阶段实施文档用于补充细节与追溯设计来源。

MCP、诊断与 AI Agent 专项文档已统一为总分结构：[文档入口](stages/P8_AI_MCP_Interface/README.md)、[总体架构](stages/P8_AI_MCP_Interface/00_Overall_Architecture.md)、[阶段划分与验收](stages/P8_AI_MCP_Interface/01_Phases_and_Acceptance.md)。各阶段实施计划位于 stages/P8_AI_MCP_Interface/；原项目P8的当前契约、完整验收记录和历史设计已分别归入mcp/02、03、04。


## 阅读顺序

1. [总体架构](../ARCHITECTURE.md)：系统边界、数据流、线程、所有权和目标架构。
2. [配置、Profile 与主题](Configuration_Profile_Theme.md)：配置分层、Profile、Session 和主题职责。
3. [渲染架构](Rendering_Architecture.md)：Snapshot、调度、命令缓存、QRhi 和 Glyph 系统。
4. [阶段路线图](Development_Roadmap.md)：P0～P9 的依赖关系、状态和统一指标。
5. `stages/`：每个阶段的独立实施说明。
   - 对照参考：[Windows Terminal 渲染架构](Windows_Terminal_Rendering_Architecture.md)
     （基于本地源码 `D:\qt\terminal` 的 Renderer/AtlasEngine 全链路分析，供渲染优化对照）。
6. [MCP当前接口契约](stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md)：进程内 Streamable HTTP、会话身份、
   独立读取/输入授权、九工具契约、预算与验收；基础观察/操作闭环已接入；[完整验收记录](stages/P8_AI_MCP_Interface/03_Implementation_and_Verification.md)与[历史设计档案](stages/P8_AI_MCP_Interface/04_Historical_Design_Archive.md)分开保存。
7. [Windows 串口压力验收](Performance_Serial_Stress_2026-10-07.md)：虚拟串口
   数据量矩阵、实际驱动队列查询与 256 KiB 大队列复测；
   [CPU 热点采样](Performance_Serial_Hotspots_2026-10-07.md)记录 WPR/xperf
   的线程分离与函数占比。两者为验收记录，不改变阶段完成状态。
8. [LZ4 历史压缩实施计划](LZ4_Scrollback_Implementation_Plan_2026-10-17.md)：
   P4 增量特性，覆盖无损分块编码、拥有型读取、后台压缩、内存预算与验收。
   已接入可选压缩与完整无 Qt 后端；默认关闭，性能发布门槛与长稳/跨平台验收未完成。

## 阶段文档

| 阶段 | 状态 | 文档 |
| --- | --- | --- |
| P0 基线与构建拆分 | 已完成（2026-07-29） | [P0](stages/P0_Baseline_and_Build.md) |
| P1 ScreenBuffer 与 VTAdapter | 退出标准已达成（2026-07-29 架构边界落地）；已完成增量：宽字符 continuation 回归（2026-09-03）、chunk 快照碎片化成本优化（尾部增量窄接口，2026-09-08）；剩余两个不阻塞边角项——活动屏幕不参与搜索、`dim`/`protectedCell` 因 vendored libvterm screen 层无来源 | [P1](stages/P1_ScreenBuffer_and_VTAdapter.md) |
| P2 异步 Parser 与背压 | 功能与 20 MiB/s 性能目标完成（2026-08-01） | [P2](stages/P2_Async_Parser_and_Backpressure.md) |
| P3 增量渲染 | 实现与 Vulkan/OpenGL 实机 60 FPS 跑分完成（2026-08-01）；待高刷新率、资源恢复与人工视觉验收 | [P3](stages/P3_Incremental_Rendering.md) |
| P4 Chunked Scrollback 与搜索 | 原 P4 已完成（2026-08-01）；LZ4 可选增量已接入，性能/长稳验收未完成，默认关闭（2026-10-19） | [P4](stages/P4_Chunked_Scrollback_and_Search.md) |
| P5 Glyph 与 GPU 管线 | 实施完成；Linux Vulkan/OpenGL、Windows D3D11/D3D12 与 30 分钟长稳验收完成（2026-08-02）；macOS Metal、多屏 DPR 与 120/144 Hz 验收待完成 | [P5](stages/P5_Glyph_and_GPU_Pipeline.md) |
| P6 Session 与 Transport | 进行中：Transport 四种完成，会话编排采用「1 View 拥有 1 Session」已在生产；剩 keyboard-interactive、close 模式、exited→UI、ProfileStore 持久化、contract tests | [P6](stages/P6_Session_and_Transport.md) |
| P7 系统资源查询 | 已实现为内置功能（常驻监控 2026-09-06、系统信息窗口 2026-09-09）；性能量化验收待补 | [P7](stages/P7_System_Resource_Monitor.md) |
| P8 AI MCP 接口 | HTTP 九工具及基础终端观察/操作闭环已实现（2026-10-17）；托管提权适配器、完整命令跟踪和 4 Hz 合并待收口；历史响应率 94.2% 未达 99%，GUI frame P95、跨平台/真实 Shell 验收未完成；mcp/P1 能力目录/类型化端口与路由已落地，Linux 范围见 [P1验收](stages/P8_AI_MCP_Interface/P1_Implementation_and_Verification.md) | [MCP契约](stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md) |
| P9 文件传输协议 | 协议库及串口 Session/手动传输窗口已接入；Linux 纯协议、门面、UI 与真实 QSerialPort/raw PTY 通路测试通过；Windows 取消发布竞争用例有已知失败，macOS 与真实 UART 文件传输验收待补 | [P9](stages/P9_File_Transfer_Protocols.md) |

## 文档权威性

- `docs/ARCHITECTURE.md` 描述当前统一设计和后续目标；本目录文档提供配套细节。
- P0 基线数值见 [P0](stages/P0_Baseline_and_Build.md) 的“基线结果”一节。原
  `docs/P0_Performance_Baseline.md` 已在架构文档合并时删除。
- 原 `P1/P2/P3` 文档是实施日志，数值和变更清单仍有追溯价值。
- 早期概念设计（原 `docs/01_Architecture.md`）已并入
  [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md) 的**附录 A**，仅供追溯；与该文档正文
  或当前源码冲突时，以正文和源码为准。原 `NovaTerm开发指南.md` 与旧
  QRhi/Profile/Theme 概念文档已随合并删除，不再作为依据。
- 状态只有在代码、自动化测试和相应验收完成后才能改为“已完成”。功能完成但性能未达标时必须分别标注。
- 各阶段状态以 `stages/P*.md` 自身的状态行为准；上方阶段表是同步过去的副本，
  不一致时以阶段文档为真。

## 统一术语

| 术语 | 含义 |
| --- | --- |
| Cell | NovaTerm 自有终端单元，不是 `VTermScreenCell` |
| ScreenBuffer | Parser 可写的活动屏幕模型 |
| TerminalSnapshot | 发布给消费者的稳定只读视图 |
| DirtyRegion | Cell 坐标系中的脏区域 |
| Session | 一条连接及其 Parser、模型和生命周期的组合 |
| Profile | 创建 Session 的持久化模板，不是运行中 Session |
| Terminal Scheme | ANSI、前景、背景、光标、选择颜色集合 |
| UI Theme | 应用窗口和控件外观，不控制终端 ANSI 语义 |
| MiB/s | 以 1 MiB = 1,048,576 bytes 计算的吞吐量 |
| CoreTypes | `src/core/CoreTypes.h`：核心层不依赖 Qt 的基础类型（`isize`/`u8`/`u32`/`u64`/`ByteView`），与 `qsizetype`/`quint*`/`QByteArrayView` 同义但不引入 Qt |
| 门面层 / coreqt | 承载 Qt 依赖（QObject 信号、QString、QKeyEvent、QRegularExpression）的层。当前即 `TerminalCore` 的门面部分；计划中的独立目录 `src/coreqt/` 尚未创建（见 `ARCHITECTURE.md` §3.4）|
