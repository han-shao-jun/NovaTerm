# NovaTerm MCP、诊断与 AI Agent 文档集

更新日期：2026-10-18。结构：总体架构 → 阶段划分 → 各阶段实施计划。

本目录整合原运行期内核重构设计、实施计划、MCP 诊断扩展 v1.1 草案、项目P8接口及完整实施记录，以及 Netcatty Agent 架构调研。此次仅整理文档，不执行功能开发，不改变已发布工具、授权或部署方式。

## 阅读入口

1. [总体架构](00_Overall_Architecture.md)：统一术语、系统分层、数据/控制流、身份授权、线程、预算和外部契约。
2. [阶段划分与验收](01_Phases_and_Acceptance.md)：依赖、当前状态、旧 W/M 阶段映射、实施顺序和统一完成定义。
3. [当前MCP接口契约](02_Current_MCP_Contract.md)：九工具、HTTP/双协议、字段/annotations、权限、观察/输入/脚本、错误与预算。
4. [实施与验收记录](03_Implementation_and_Verification.md)：按日期/平台保存原P8的完整实施、审查和测量证据；[历史设计档案](04_Historical_Design_Archive.md)只作追溯。
5. 按阶段阅读下列实施计划。P0～P8 是本目录内部阶段编号，与上级项目 P0～P9 不同；引用时写“mcp/Pn”。

| 阶段 | 计划 | 状态 |
| --- | --- | --- |
| mcp/P0 | [运行内核收敛](P0_Runtime_Consolidation.md) | 实现与结构收口完成，Linux专项通过；其他平台范围见P0验收表 |
| mcp/P1 | [能力目录、应用入口与执行路由](P1_Capability_Runtime.md) | 规划，尚未实现通用跨入口 Runtime |
| mcp/P2 | [保真历史录制与存储](P2_History_Recording.md) | 规划，录制默认关闭 |
| mcp/P3 | [历史搜索、证据与离线授权](P3_History_Search_and_Evidence.md) | 规划 |
| mcp/P4 | [Backend 诊断](P4_Backend_Diagnostics.md) | 规划 |
| mcp/P5 | [Embedded Linux 诊断](P5_Embedded_Diagnostics.md) | 规划，离线解析优先 |
| mcp/P6 | [内置 Agent 与聊天 UI](P6_Agent_Runtime_and_UI.md) | 规划；基础聊天不依赖历史录制或诊断包全部完成 |
| mcp/P7 | [外部 Agent 与产品能力扩展](P7_External_Agents_and_Product_Tools.md) | 规划，逐后端/逐能力交付 |
| mcp/P8 | [集成、性能与跨平台验收](P8_Integration_and_Performance.md) | 持续验证；综合退出条件尚未达成 |

## 核心架构要求

**低耦合、高内聚为所有阶段的强制约束**：按职责和不变量归属状态，以窄契约单向协作，组合根只装配、Dispatcher只分派。不得以拆文件、通用Manager、巨大Ports或全局事件总线替代模块边界。具体依赖矩阵与验收见[总体架构](00_Overall_Architecture.md)§2.1～§2.4和[阶段验收](01_Phases_and_Acceptance.md)§7。

## 文档权威与状态

- 全项目原则仍以 [统一架构](../../../ARCHITECTURE.md)、当前源码和已验证接口为准。
- 当前MCP精确契约见[接口契约](02_Current_MCP_Contract.md)，用户操作见[MCP使用说明](../../../MCP_Usage.md)；完整实施证据见[验收记录](03_Implementation_and_Verification.md)，被替代设计见[历史档案](04_Historical_Design_Archive.md)。原项目P8不再承担文档入口。
- 本目录是 MCP/诊断/Agent 后续架构与阶段实施的统一入口；总体架构定义共性，阶段计划定义局部交付。
- “已有代码”“历史测试记录”“尚未实施的设计”分别表述。旧计划中的未完成条目若已被后续 W5 记录覆盖，不再作为当前待办。
- 不因组件已拆文件就宣布原目标全部完成；本次请求所有权/确认/只读边界收敛及wait/replay、执行入口和平台剩余范围见mcp/P0。
- 所有未来类型、文件和测试目标均为计划，只有实际加入构建并验证后才能改为已实现。
- 历史测试数字不代表此次重新运行；本次文档整理不产生新的构建、性能或平台通过结论。

## 来源迁移与删除范围

| 原文档 | 内容去向 |
| --- | --- |
| docs/superpowers/specs/2026-10-18-mcp-runtime-refactor-design.md | 总体架构、mcp/P0 的兼容/所有权/状态机/删除与结构验收 |
| docs/superpowers/plans/2026-10-18-mcp-runtime-refactor.md | mcp/P0 的 W1～W5 状态、目标库、迁移去向、验证与遗留差距 |
| docs/NovaTerm_MCP_Diagnostics_Design.md | 总体架构及 mcp/P1～P5、mcp/P8 的能力/历史/证据/诊断/性能计划 |
| docs/architecture/stages/P8_AI_MCP_Interface.md | 02当前契约（源码/fixture核对）、03完整实施/验收记录、04原设计档案；P0/P8吸收当前结构及性能验收要求 |
| Netcatty 本地源码调研及本次会话总结 | 总体架构及 mcp/P6、mcp/P7 的回合、事件、工具、上下文、权限和 UI |

完成内容、分区覆盖与引用核对后，原三份设计和原项目P8均可移除；P8的历史内容已分区保存在03/04，不依赖旧文件。MCP使用说明、其他文档和测试fixture不在删除范围。表中旧名称仅用于来源追溯，不作为有效路径链接。

Netcatty 参考包括 AgentRuntime、TurnDriver、AgentEvent、Capability Catalog、ToolOutputStore、AIChatSidePanel。借鉴语义边界，不移植 Electron/Node 拓扑，不把其实验 App Server 或未来全局 Agent 写成 NovaTerm 已有能力。

## 2026-10-18 设计补齐索引

本轮只补设计与未来验收，不改变当前实现状态或02接口契约：

| 评审缺口 | 决策与任务位置 |
| --- | --- |
| 模型发送、摘要/历史再次使用授权 | [总体§11](00_Overall_Architecture.md#11-设计补充契约2026-10-18尚未实现)、[P6发送契约](P6_Agent_Runtime_and_UI.md#补充契约模型发送与派生内容授权)、[P7外部后端](P7_External_Agents_and_Product_Tools.md#补充实施约束外部后端的数据发送) |
| 环境revision的来源与失效 | [P1环境契约](P1_Capability_Runtime.md#补充契约环境事实与失效)，P4/P5消费 |
| 崩溃后的未知尾部与持久前缀 | [P2提交契约](P2_History_Recording.md#补充契约持久提交与异常尾部) |
| 搜索续页的decoder/query/租约 | [P3续扫契约](P3_History_Search_and_Evidence.md#补充契约搜索续页与有界状态) |
| 异构领域的类型化分派 | [P1分派契约](P1_Capability_Runtime.md#补充契约领域类型化分派) |
| 首个交付前置范围 | [阶段划分§6.1](01_Phases_and_Acceptance.md#61-增量交付闸门)、[P8补充矩阵](P8_Integration_and_Performance.md#补充验收矩阵跨阶段契约) |

物理目录统一为docs/architecture/stages/P8_AI_MCP_Interface/；mcp/P0～P8继续作为专项阶段简称，不对应另一个mcp目录。历史档案不改写成当前设计。
