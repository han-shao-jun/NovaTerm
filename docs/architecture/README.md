# NovaTerm 架构文档索引

> 文档集版本：2.0  
> 整理日期：2026-07-31  
> 适用项目版本：NovaTerm 0.1.x 及后续重构阶段

本目录是 NovaTerm 当前的统一架构入口。`docs/` 根目录中的早期设计、性能记录和阶段实施文档继续保留，用于追溯设计来源与历史数据；新开发和架构评审应优先引用本目录。

## 阅读顺序

1. [总体架构](NovaTerm_Architecture.md)：系统边界、数据流、线程、所有权和目标架构。
2. [配置、Profile 与主题](Configuration_Profile_Theme.md)：配置分层、Profile、Session 和主题职责。
3. [渲染架构](Rendering_Architecture.md)：Snapshot、调度、命令缓存、QRhi 和 Glyph 系统。
4. [阶段路线图](Development_Roadmap.md)：P0～P7 的依赖关系、状态和统一指标。
5. `stages/`：每个阶段的独立实施说明。

## 阶段文档

| 阶段 | 状态 | 文档 |
| --- | --- | --- |
| P0 基线与构建拆分 | 已完成 | [P0](stages/P0_Baseline_and_Build.md) |
| P1 ScreenBuffer 与 VTAdapter | 已完成 | [P1](stages/P1_ScreenBuffer_and_VTAdapter.md) |
| P2 异步 Parser 与背压 | 已完成，性能继续优化 | [P2](stages/P2_Async_Parser_and_Backpressure.md) |
| P3 增量渲染 | 实现完成，待实机验收 | [P3](stages/P3_Incremental_Rendering.md) |
| P4 Chunked Scrollback 与搜索 | 计划中 | [P4](stages/P4_Chunked_Scrollback_and_Search.md) |
| P5 Glyph 与 GPU 管线 | 计划中 | [P5](stages/P5_Glyph_and_GPU_Pipeline.md) |
| P6 Session 与 Transport | 计划中 | [P6](stages/P6_Session_and_Transport.md) |
| P7 插件与扩展 | 计划中 | [P7](stages/P7_Plugin_System.md) |

## 文档权威性

- 本目录描述当前统一设计和后续目标。
- `P0_Performance_Baseline.md` 是 P0 原始测量记录。
- 原 `P1/P2/P3` 文档是实施日志，数值和变更清单仍有追溯价值。
- `NovaTerm开发指南.md`、旧 QRhi/Profile/Theme 文档属于早期概念设计；与本目录冲突时，以本目录和当前源码为准。
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

