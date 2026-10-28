# 实施与验证证据档案

本目录只保存已发生的实现、审查、测试、失败和限制，不保存被放弃的实施方案。旧 mcp/P 编号和“当前”均按记录日期理解，不代表当前工作区已通过，不要求继续旧平台路线。

当前源码为二十九工具，UART/ZMODEM上传、查询与取消本阶段正在验证；SFTP下载、显式覆盖和平台收尾见[软件收口账本](../08_Software_Closeout.md)，B4由用户实测。下表按各记录当时范围解释，不能将旧子批“待后续”当作当前能力结论。

当前入口：[总体架构](../00_Overall_Architecture.md)、[缺口](../03_Gaps_and_Evidence.md)、[实施与验收](../01_Phases_and_Acceptance.md)。

| 记录 | 保留原因 |
| --- | --- |
| [UART/ZMODEM单文件发送](B3b_Uart_Send_2026-10-28.md) | 独立本地read+UART授权、原Controller/worker复用、原ID取消、有限期限及协议/文件确认事实；本阶段验证进行中 |
| [UART独立授权目录](B3b_Uart_Authorization_2026-10-28.md) | 本机显式发送授权及当前连接只读发现，旧代际/配置/身份回收；保留当时仅授权目录的边界和验证 |
| [UART安全文件源前置](B3b_Uart_File_Source_2026-10-28.md) | 同句柄随机读、现有串口worker源工厂与ZMODEM替身通路；保留当时尚无公开UART工具的前置验证 |
| [B3b SFTP行为审查修复](B3b_Sftp_Review_Fixes_2026-10-28.md) | 自然退出回收未执行上传及后端销毁后保留已知进度，逐轮RED/GREEN与本轮验证 |
| [B3b SFTP单文件上传](B3b_Sftp_Upload_2026-10-27.md) | 2026-10-27～28工作区上传/查询/取消、复用终端SFTP、实际验证和ZMODEM下一起点 |
| [B3b 本地句柄I/O](B3b_Local_File_IO_2026-10-27.md) | 第二文件子批Windows worker安全读写、临时文件发布及取消；未接MCP/SFTP，二十二工具保持 |
| [B3b 文件资源授权目录](B3b_File_Resources_2026-10-27.md) | 文件传输第一子批独立本地/远端读写授权及只读元数据发现；文件内容传输仍待后续 |
| [B3b 新会话预授权](B3b_New_Session_Sharing_2026-10-27.md) | 第四子批本机逐客户端规则、原连接就绪后一次授权及撤回/重试/人工接管边界；文件仍待后续 |
| [B3b 保存目标安全关闭](B3b_Target_Close_2026-10-27.md) | 第三子批原创建操作归属、人工接管、忙碌/未知事实与精确重试；不关闭复用会话、不删除终端 |
| [B3b 保存目标打开与查询](B3b_Target_Connections_2026-10-27.md) | 第二子批独立打开授权、可见终端复用/创建、幂等操作和连接查询；关闭/传输仍待后续 |
| [B3b 保存目标授权目录](B3b_Target_Directory_2026-10-27.md) | 第一子批只读目录、配置/身份绑定、GUI共享与真实RED/GREEN；打开/关闭和文件传输仍待后续 |
| [B3/B3a 审查修复](B3_Review_Fixes_2026-10-27.md) | 新token下普通输入Replay拒绝、新任务真实Submit关联及慢轮询公平性；不修改无收益机械风格 |
| [B3/B3a 基础恢复、文本等待与长任务](B3_Implementation_2026-10-27.md) | 四工具契约、现有owner复用、独立审查修复及本轮Windows替身/构建成绩；不含B3b/真机 |
| [B2 审查修复](B2_Review_Fixes_2026-10-26.md) | expand 精确错误码、输入提交边界核验及本轮专项结果 |
| [B2 清空与代际](B2_Implementation_2026-10-25.md) | 独立权限、Parser 完成、有界幂等与 G05 失效，本轮验收/限制 |
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
