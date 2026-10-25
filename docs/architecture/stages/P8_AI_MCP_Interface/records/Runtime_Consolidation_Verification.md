# 运行内核收敛历史证据

> 历史实施与验证证据；保留原日期、命令、失败和限制。不是当前实施计划，旧 P 编号仅用于追溯。当前路线见 [目录入口](../README.md)。

## 1. 已落地 W1～W5 与证据

以下为源码及03实施与验收记录 截至 2026-10-18 的历史记录，不是此次重跑结果。

| 波次 | 历史落地内容 | 当时文件/接口（当前去向见§2） |
| --- | --- | --- |
| W1 | 强类型 ID/绑定/错误/预算、九请求/六动作；Schema 与解码迁出 | contracts/*；protocol/ToolCodec，decode/catalog/encodeFailure |
| W2a | 单项权限事务、级联、行/客户端版本、删除整组回写与死字段 | AuthorizationService::change、GrantStore、Permissions.h |
| W2b | ClientRegistry、统一能力解析、只读授权 UI 模型 | security/ClientRegistry；CapabilityResolver/Adapter；SessionPermissionModel/SettingsController |
| W3a | 统一基础采样和两 scope capture 存储 | SnapshotStore、CaptureStore、ObservationService |
| W3b | 读取/搜索/投影和统计收敛 | ObservationService::read、CaptureSearch、ProtocolProjectionSizer |
| W4a | Session automation 独立目标；确认注册表代替五张表 | src/mcp/CMakeLists.txt；ConfirmationRegistry |
| W4b/c | 每 peer 调度、执行记录、move-only 目标预留 | RequestScheduler、ExecutionRegistry、TargetReservation |
| W5a～f | typed backend、脚本任务、typed capture 行、去重、证据真源、在途注册表 | BackendEvents、ScriptTaskRegistry、CaptureLine、CommandDedup、TerminalEvidenceModel、TaskRegistry |
| W5g | typed TerminalInteraction，删除 Job::arguments | TerminalInteraction::call(ToolRequest)、ToolCodec::encodeActions |
| W5h/i | 只读与固定诊断用例、阶段单向推进 | ReadUseCases、DiagnosticCommandUseCase、TaskPhases |
| W5j/k/l | 脚本结果/主流程及执行终态迁出 Service | ScriptResultUseCase、ScriptCommandUseCase、ExecutionService::finish |

兼容 fixture 由重构前 tools() 导出，143 KiB，路径 tests/mcp/fixtures/tool-catalog-2026-10-18.json，不是手写样本。
旧基线 f067c91 + 用户未提交改动：Linux MCP 七项 7/7（29.81 秒）。
后续 W5l：Linux MCP 九项 9/9（28.36 秒），全套 27/29（190.26 秒）；openpty 环境导致 Session 串口 PTY 与 novaterm_pty_tests 失败。Windows/macOS、真实 UART/SSH、可信提权适配器和 Windows 文件竞争未验收。
完整逐波记录继续保存在 [实施与验收记录](03_Implementation_and_Verification.md)，不以早期“未完成”覆盖后期结果。

## 2. 当前收口状态（2026-10-18，P0实现与Linux专项验收完成）

T1～T5实现及结构检查已收口；Linux主程序、12项MCP和Session专项通过。Windows/macOS、真实UART/SSH/TUI、托管提权适配器和Windows文件竞争不计入本轮通过范围，继续在平台验收矩阵追踪。

| 职责 | 当前所有者与边界 | 退出证据 |
| --- | --- | --- |
| RPC接纳、调度、交付 | Service唯一持有PendingRequest；WireSession只向后端发送ToolRequest/类型化确认答复及摘要，不再转发JSON业务参数 | 契约fixture、wire/endpoint、重复RPC ID和取消回归 |
| 执行、发布、确认、取消 | CommandRuntime只依赖Qt Core及类型化CommandSessionPort/CommandTargetStore；私有操作、执行记录、路径锁和注册表由其唯一修改 | CommandRuntimeTests独立链接application/Test；同步终态、撤权、owner销毁、迟到发布和未知保护 |
| Session/持久化适配 | CommandSessionAdapter拥有具体Session信号、通道ID、TargetGuard及JSON投影；不决定任务阶段或复制任务状态 | 既有九工具集成、脚本、输入和重启保护回归 |
| 等待、重放、输入序号 | ObservationState拥有等待准入/替换/到期/撤销、输入重放及注意提示去重；TerminalInteraction只做Session事实与token/JSON投影适配 | ObservationStateTests独立链接application/Test；32等待者预算、150ms静默窗、绑定、重放和取消 |
| 基础采样/capture | ObservationService仍是唯一SnapshotStore/CaptureStore消费入口；新状态组件不建立第二采样源 | 250ms合并、epoch、两scope配额/TTL、full/compact截断和补读回归 |
| 只读规则 | ReadUseCases通过ReadSessionPort消费不可变事实，CapabilityAdapter负责具体接线 | ReadUseCasesTests独立链接application/Test |
| 停机与worker | 搜索关闭受锁保护邮箱后在后台回收；凭据worker不等待GUI析构且secret RAII擦除；HTTP关闭邮箱后最多等待1000ms，异常线程自行收尾 | 阻塞假凭据后端析构RED→GREEN，HTTP关闭/重启与12项MCP回归 |

上一轮删除17份旧生产文件、增加CommandRuntime两份，生产文件78→63；本轮为真实Session适配边界及观察状态所有者增加两对文件，当前**67份，较最初仍净减少11份**。没有新增通用Manager、Dispatcher或空壳Broker。

原ScriptCommandUseCase、ScriptResultUseCase、DiagnosticCommandUseCase、ExecutionService、ExecutionRegistry、ScriptTaskRegistry、TaskRegistry、Job和ScriptInvocation均已删除；历史W1～W5条目的旧名称仅作追溯。

共享业务入口、能力目录和新增执行Recipe属于P1。P0的现有九工具执行/观察已可通过真实最小端口独立验证，不以提前实现P1通用框架作为完成条件。
