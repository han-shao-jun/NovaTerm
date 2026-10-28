# NovaTerm 架构文档索引

2026-10-28 UART安全文件源前置已接入现有串口worker并补真实ZMODEM替身回归；独立UART授权与MCP入口仍待后续，二十五工具保持，见[安全文件源记录](stages/P8_AI_MCP_Interface/records/B3b_Uart_File_Source_2026-10-28.md)。

2026-10-28 工作区受限SFTP上传/查询/取消已接入，当前二十五工具；复用终端原SFTP后端，本地句柄Reader已有生产调用，见[上传记录](stages/P8_AI_MCP_Interface/records/B3b_Sftp_Upload_2026-10-27.md)。

2026-10-27 文件传输第二子批新增Windows本地句柄I/O库，供后续传输worker使用；未接MCP/SFTP、不新增工具，见[记录](stages/P8_AI_MCP_Interface/records/B3b_Local_File_IO_2026-10-27.md)。

2026-10-27 B3b文件传输第一子批：新增独立本地/远端读写授权与只读 `novaterm_list_file_resources`，当前二十二工具。仅发现已授权元数据，本机路径与文件内容不回传，上传/下载/进度/取消和安全发布仍待后续，见[文件资源记录](stages/P8_AI_MCP_Interface/records/B3b_File_Resources_2026-10-27.md)。

2026-10-27：新增 `novaterm_close_target`，当前二十一工具；仅关闭原操作创建且未人工接管、具有空闲证据的连接，保留终端。单文件传输和B4待后续，见[安全关闭记录](stages/P8_AI_MCP_Interface/records/B3b_Target_Close_2026-10-27.md)。

> 文档集版本：2.1  
> 整理日期：2026-09-02（本表状态以各阶段文档自身的状态行为准，已同步）  
> 适用项目版本：NovaTerm 0.1.x 及后续重构阶段

本目录收录 NovaTerm 的架构配套文档。统一架构总览位于 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md)；本目录中的配置、渲染、路线图和阶段实施文档用于补充细节与追溯设计来源。

外部 Agent 的 SSH/UART MCP 专项入口：[文档导航](stages/P8_AI_MCP_Interface/README.md)、[总体架构](stages/P8_AI_MCP_Interface/00_Overall_Architecture.md)、[闭环缺口](stages/P8_AI_MCP_Interface/03_Gaps_and_Evidence.md)、[实施与验收](stages/P8_AI_MCP_Interface/01_Phases_and_Acceptance.md)。2026-10-24 已删除旧 mcp/P0～P8 平台方案，采用 B0～B4 终端闭环任务；实施证据独立保存在 records/。


## 阅读顺序

2026-10-27 MCP更新见 [工作流扩展](stages/P8_AI_MCP_Interface/06_Terminal_Workflow_Extensions.md)：B1日志、B2清空及B3/B3a基础恢复、条件等待和长任务已实施，B3b已提供显式共享的保存目标目录、独立打开/复用/查询、安全关闭及新会话预授权，当前二十一工具；单文件传输仍未实施。源码、替身与真实设备的验收范围分开，本批见 [预授权记录](stages/P8_AI_MCP_Interface/records/B3b_New_Session_Sharing_2026-10-27.md)；设备测试见[独立清单](stages/P8_AI_MCP_Interface/07_Real_Device_Test_Checklist.md)。

1. [总体架构](../ARCHITECTURE.md)：系统边界、数据流、线程、所有权和目标架构。
2. [配置、Profile 与主题](Configuration_Profile_Theme.md)：配置分层、Profile、Session 和主题职责。
3. [渲染架构](Rendering_Architecture.md)：Snapshot、调度、命令缓存、QRhi 和 Glyph 系统。
4. [阶段路线图](Development_Roadmap.md)：P0～P9 的依赖关系、状态和统一指标。
5. `stages/`：每个阶段的独立实施说明。
   - 对照参考：[Windows Terminal 渲染架构](Windows_Terminal_Rendering_Architecture.md)
     （基于本地源码 `D:\qt\terminal` 的 Renderer/AtlasEngine 全链路分析，供渲染优化对照）。
6. [MCP当前接口契约](stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md)：进程内 Streamable HTTP、会话身份、
   独立读取/输入授权、九终端工具与三历史工具的兼容基线；基础观察/操作已接入，B1 精确日志游标和 B2 独立授权清空已实施。[验收档案](stages/P8_AI_MCP_Interface/records/README.md)按日期记录已有成绩。
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
| P8 AI MCP 接口 | B0/B1、B2、B3/B3a及B3b保存目标连接、新会话预授权、独立文件授权和受限SFTP上传/查询/取消已实施，当前二十五工具。UART安全文件源前置已提供；UART授权/工具、下载/覆盖及真实SSH/UART双通道/客户端/性能仍待后续，见 [缺口表](stages/P8_AI_MCP_Interface/03_Gaps_and_Evidence.md) | [MCP契约](stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md) |
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

当前模块边界另见 [历史模块目录重构](History_Module_Layout_2026-10-25.md)：LZ4/录制/离线查询分置独立目录，MCP 可以引用，具体源适配属于组合边界。
