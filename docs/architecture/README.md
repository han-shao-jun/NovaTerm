# NovaTerm 架构文档索引

> 文档集版本：2.1  
> 整理日期：2026-09-02（本表状态以各阶段文档自身的状态行为准，已同步）  
> 适用项目版本：NovaTerm 0.1.x 及后续重构阶段

本目录收录 NovaTerm 的架构配套文档。统一架构总览位于 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md)；本目录中的配置、渲染、路线图和阶段实施文档用于补充细节与追溯设计来源。

## 阅读顺序

1. [总体架构](../ARCHITECTURE.md)：系统边界、数据流、线程、所有权和目标架构。
2. [配置、Profile 与主题](Configuration_Profile_Theme.md)：配置分层、Profile、Session 和主题职责。
3. [渲染架构](Rendering_Architecture.md)：Snapshot、调度、命令缓存、QRhi 和 Glyph 系统。
4. [阶段路线图](Development_Roadmap.md)：P0～P7 的依赖关系、状态和统一指标。
5. `stages/`：每个阶段的独立实施说明。

## 阶段文档

| 阶段 | 状态 | 文档 |
| --- | --- | --- |
| P0 基线与构建拆分 | 已完成（2026-07-29） | [P0](stages/P0_Baseline_and_Build.md) |
| P1 ScreenBuffer 与 VTAdapter | 架构边界已完成（2026-07-29）；宽字符 continuation 与部分属性映射待补完 | [P1](stages/P1_ScreenBuffer_and_VTAdapter.md) |
| P2 异步 Parser 与背压 | 功能与 20 MiB/s 性能目标完成（2026-08-01） | [P2](stages/P2_Async_Parser_and_Backpressure.md) |
| P3 增量渲染 | 实现与 Vulkan/OpenGL 实机 60 FPS 跑分完成（2026-08-01）；待高刷新率、资源恢复与人工视觉验收 | [P3](stages/P3_Incremental_Rendering.md) |
| P4 Chunked Scrollback 与搜索 | 已完成（2026-08-01） | [P4](stages/P4_Chunked_Scrollback_and_Search.md) |
| P5 Glyph 与 GPU 管线 | 实施完成；Linux Vulkan/OpenGL、Windows D3D11/D3D12 与 30 分钟长稳验收完成（2026-08-02）；macOS Metal、多屏 DPR 与 120/144 Hz 验收待完成 | [P5](stages/P5_Glyph_and_GPU_Pipeline.md) |
| P6 Session 与 Transport | 进行中：Transport 完成，编排层未接入 | [P6](stages/P6_Session_and_Transport.md) |
| P7 插件与扩展 | 计划中（依赖核心数据通路与 Session API 稳定） | [P7](stages/P7_Plugin_System.md) |

## 文档权威性

- `docs/ARCHITECTURE.md` 描述当前统一设计和后续目标；本目录文档提供配套细节。
- `P0_Performance_Baseline.md` 是 P0 原始测量记录。
- 原 `P1/P2/P3` 文档是实施日志，数值和变更清单仍有追溯价值。
- `NovaTerm开发指南.md`、旧 QRhi/Profile/Theme 文档属于早期概念设计；与统一架构文档和当前源码冲突时，以后者为准。
- 状态只有在代码、自动化测试和相应验收完成后才能改为“已完成”。功能完成但性能未达标时必须分别标注。

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

