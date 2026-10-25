# 实施与验证证据档案

本目录只保存已发生的实现、审查、测试、失败和限制，不保存被放弃的实施方案。旧 mcp/P 编号和“当前”均按记录日期理解，不代表当前工作区已通过，不要求继续旧平台路线。

当前入口：[总体架构](../00_Overall_Architecture.md)、[缺口](../03_Gaps_and_Evidence.md)、[实施与验收](../01_Phases_and_Acceptance.md)。

| 记录 | 保留原因 |
| --- | --- |
| [B0/B1 日志与清理](B0_B1_Implementation_2026-10-25.md) | 当前日志契约、P4/P1 退役、兼容和本轮验证 |
| [MCP 历次实施](03_Implementation_and_Verification.md) | 原终端工具、HTTP、授权、输入/取消、快照和性能的逐次证据 |
| [运行内核收敛](Runtime_Consolidation_Verification.md) | 从旧 P0 提取已落地与验收片段；其余实施计划已删除 |
| [能力运行期](P1_Implementation_and_Verification.md) | 已有应用入口、环境/任务所有权与专项验证 |
| [录制](P2_Implementation_and_Verification.md) | 私有存储、配额、恢复、关闭及 Windows 竞争证据 |
| [离线共享与历史搜索](P3_Implementation_and_Verification.md) | 独立授权、历史接口、证据和平台范围 |
| [P3 审查修复](P3_Review_Fixes_2026-10-23.md) | 已修问题与对应测试，避免重引入 |
| [P3 资源边界修复](P3_Architecture_Fixes_2026-10-24.md) | 取消、来源生命周期和应用/协议分离 |
| [P4 暂停实现](P4_Implementation_and_Verification.md) | 未验收工作区改动及已知失败；不是继续实施的计划 |
| [本次文档重构](Document_Refactor_2026-10-24.md) | 目录处置、源码核对范围与验证方式 |

原历史设计档案已删除，不再维护另一套旧规范。历史证据中提到的旧类名、旧工具数量、旧步骤编号和拟议事项均只反映当时上下文；今天应做什么只看目录外的当前文档。

2026-10-25 后续目录整理见 [历史模块重构](../../../History_Module_Layout_2026-10-25.md)。旧记录中的 src/history、tests/history 路径按当时版本理解，当前录制/离线查询分别位于 recording/historysearch；不据目录移动删除原始成绩。
