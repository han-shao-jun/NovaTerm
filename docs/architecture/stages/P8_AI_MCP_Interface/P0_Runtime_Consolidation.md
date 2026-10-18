# mcp/P0 运行内核收敛实施计划

> For agentic workers：实施时使用 superpowers:executing-plans，逐任务核验；本文件的已落地部分不重新开发，未完成任务以复选框追踪。

**Goal:** 保持九工具外部行为，收敛内部类型、状态所有者、依赖和安全边界，为共享能力入口建立可信基线。
**Architecture:** ToolCodec → 类型化请求/事件 → 现有应用用例 → Session 自动化；协议仅做适配。已落地 W1～W5 先核实，剩余结构差距用逐用例替换收口。
**Tech Stack:** C++17、Qt 6.8.3、Qt Test、现有 HTTP/MCP、CMake。
**Spec:** [总体架构](00_Overall_Architecture.md)；[阶段与完成定义](01_Phases_and_Acceptance.md)。

## Global Constraints

九工具名称/Schema/错误/确认/取消保持；不双路或影子执行副作用；不升级授权；不恢复 stdio。按当前未提交工作树工作，不 reset/自动提交/另建 worktree，不清理原 core/匹配器产物。真实设备和提权适配器不在此阶段验收范围。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 旧 UI 行撤销落到重连新目标：T2 核验绑定/行版本拒绝且新授权不增加。
- 用户接管与迟到 worker/提示符竞争：T3/T4 核验零追加输入、未知结果不释放保护。
- full/compact 转换和极小 UTF-8 预算：T1/T3 核验不能推进截断游标或补读停滞。
- RPC 已结束但脚本仍在发布：T4 验证结果只收口一次，保留发布/未知事实。
- 已拆文件却仍耦合协议/Session：T5 验证纯应用目标可独立链接，不把 JSON Ports 当成已完成纯契约。

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

## 3. 文件与构建职责

已有目标：novaterm_mcp_contracts（值类型）、novaterm_mcp_protocol（wire/codec）、novaterm_mcp_http（HTTP）、novaterm_mcp_application（纯授权、读取、观察状态与命令用例等）、novaterm_session_automation（Session 执行组件）、novaterm_mcp_runtime（组合与集成）。
Session 文件多数仍在 src/session/，TerminalEvidenceModel 在 src/session/automation/；不机械搬目录。

本阶段涉及范围：src/mcp/McpService.*、TerminalInteraction.*、contracts/*、application/*、tasks/*、security/*、src/session/SessionCommandCoordinator.*、SessionTerminalController.h、automation/TerminalEvidenceModel.h、src/ui/models/*、src/mcp/CMakeLists.txt。
测试：tests/mcp/ContractsTests.cpp、AuthorizationTests.cpp、McpTests.cpp、McpAccessInteractionTests.cpp、WireSessionTests.cpp、HttpMcpEndpointTests.cpp、tests/CMakeLists.txt。
新 TokenCodec/SessionGateway/typed responses 仅在实际迁移任务需要时加入，职责分别为 typed Claims、Session 边界、纯业务结果。

## 4. 工具迁移去向

| 工具 | typed 请求 | 现有用例/后续分派 |
| --- | --- | --- |
| list_sessions | ListSessionsRequest | ReadUseCases 会话/能力视图 |
| read_context | ReadContextRequest | ReadUseCases + ObservationService / TerminalInteraction |
| search_context | SearchContextRequest | ObservationService/CaptureSearch 与相关适配 |
| list_commands | ListCommandsRequest | ReadUseCases 诊断目录 |
| execute_command | ExecuteCommandRequest | CommandRuntime::runDiagnostic |
| run_command | RunCommandRequest | CommandRuntime::runCommand |
| run_script | RunScriptRequest | CommandRuntime::runScript / finishScriptWrite |
| interact_terminal | InteractTerminalRequest | TerminalInteraction + Session controller/arbiter |
| request_elevation | RequestElevationRequest | TerminalInteraction，仍返回适配器不可用 |

九工具实际名称带 novaterm_。旧设计中的 ListDiagnosticsRequest 等概念名不得代替当前真实 ToolRequests 定义。

## 5. 实施任务

### T1：冻结兼容与状态基线

**Files:** tests/mcp/fixtures/tool-catalog-2026-10-18.json、ContractsTests.cpp、WireSessionTests.cpp、HttpMcpEndpointTests.cpp。
**Interfaces:** 消费 ToolCodec::catalog()/decode()/encodeFailure()；产出九工具对象级差分及参数集合验证报告。
- [x] 核实 fixture 对象相等，覆盖 default/optional/enum/未知字段、控制字符、UTF-8 边界，不仅数九个工具。
- [x] 核实 2025 接受/拒绝/取消/DELETE/断流，2026 MRTR 重提/篡改/到期/断流取消，重复 RPC ID。
- [x] 仅为缺失断言补失败用例；固定时钟/随机源对动态字段规范化，不删除 gap/unknown/权限断言。
- [x] 构建并跑 contracts/wire/endpoint；记录平台/环境与现有失败，不把旧成绩当新成绩。

### T2：授权和设置 UI 的单一修改路径

**Files:** application/AuthorizationService.*、GrantStore.*、CapabilityResolver.*、CapabilityAdapter.*、security/ClientRegistry.*、ui/models/*。
**Interfaces:** PermissionChange + GrantFacts → AuthorizationService::change() → GrantSnapshot/拒绝；同一能力快照供 UI/list_sessions/交互读取。
- [x] 对 stale-row、跨客户端切换、同 attachment 重连、取消读取/完整输入级联、凭据变更撤提权检查既有覆盖。
- [x] 保留固定顺序：启用→客户端→绑定→行版本→不变量；拒绝返回新快照，不整组回写。
- [x] 验证界面不取得可变 AccessStore、不在 setData/itemChanged 栈内删树，列/角色使用枚举。
- [x] 运行 authorization 与 MCP UI/凭据专项；选择可保存与后端不可用分别呈现。

### T3：观察与唯一证据源

**Files:** ObservationService.*、application/ObservationState.*、application/CaptureStore.*、CaptureSearch.*、SnapshotStore.h、protocol/ProtocolProjectionSizer.*、TerminalInteraction.*、session/automation/TerminalEvidenceModel.h。
**Interfaces:** typed ReadContextRequest/Claims→ObservationRead；Framer/input/reset→唯一 EvidenceModel；投影由协议边界测量最终大小。
- [x] 核验 250 ms 合并、epoch 键、失败不缓存、StrictRevision/BoundedStaleness、无等待者无常驻采样。
- [x] 核验连接 capture 与客户端 capture 分别配额/FIFO/60秒到期，旧搜索只查当次投影；补读不改变基础内容/续租。
- [x] 保留 full 截断不推进，compact projectionLimited/sourceGap 分离，最小 UTF-8 预算拒绝而非停滞。
- [x] 为 wait/replay 或 typed Claims 的真实缺口先写失败断言，再逐用例迁移，禁止新增第二采样/提示符缓存。
- [x] 覆盖候选静默窗150ms、非空/空编码回滚、用户抢占、多行尾部、reset、TUI 未知；运行 MCP 与相关 Session 专项。

### T4：任务所有权、确认和执行阶段收口

**Files:** McpService.cpp内的请求表、tasks/CommandRuntime.*、CommandSessionAdapter.*、ConfirmationRegistry.*、TargetReservation.*、contracts/TaskPhases.h。
**Interfaces:** typed ToolRequest/授权绑定→任务；CommandExecutionResult→CommandRuntime::finishExecution()；TaskId 定位 owner，取消交付与执行分开。
- [x] 先测取消等待后迟到发布、owner 销毁、确认过期/重放、Published-not-invoked/Unknown、预留移交和阶段非法回退。
- [x] 删除跨组件shared Job及旧TaskRegistry；请求唯一所有，命令状态私有，异步边界按ID和不可变值交互。
- [x] 固定诊断、风险命令、脚本由CommandRuntime管理预留/提交/观察/终态，覆盖同步完成不回退。
- [x] 两代协议共用业务确认事务；请求结束不删除在途事实，不在析构释放 quarantine。
- [x] 回归 unknownExecutionProtectsTarget、privateMarkersSurviveServiceRestart、cancelledCommandNeverReplays、commandsRequireSeparateGrantAndAreDeduplicated。

### T5：纯边界与删除证明

**Files:** McpService.*、tasks/ReadUseCases.*、CommandRuntime.*、protocol/BackendEvents.h、CommandSessionAdapter及现有ToolCodec/typed Claims；CMakeLists.txt 与 tests/CMakeLists.txt。
**Interfaces:** wire→BackendCall(ToolRequest)/BackendCancel/BackendElicitationResult(ConfirmationAnswer)；应用→ReadSessionPort/CommandSessionPort；typed Claims仅在集成codec边界序列化。
- [x] 针对 Ports 中 JSON/Session/key 耦合先增加可独立链接的应用测试，保留现有集成差分。
- [x] 逐用例抽取最小真实边界；JSON 留在 codec/摘要/持久化/结果投影，不在业务之间传 op/arguments。
- [x] 删除旧消费者后才删除接缝；证明无 Job::arguments、整组 grant、平行 capture/确认/脚本表或双提示符真源。
- [x] 主程序 GLOB 排除/生产库/测试清单同步，一文件一个生产消费者，不用“新类调用旧大方法”完成迁移。
- [x] 报告结构检查与未收口项；整阶段退出前全部收口，P1只读子集可按阶段划分§6.1的A闸门提前消费已收口端口。已有迁移不得恢复成永久 fallback。

## 6. 验证命令与退出

从 NovaTerm 根运行：
~~~bash
cmake --build build --config Debug
ctest --test-dir build -C Debug -L mcp --output-on-failure
ctest --test-dir build -C Debug -R novaterm_session_tests --output-on-failure
~~~
构建目录若为 Release/其他配置沿用其配置，不盲改生成器。跨 Session/Core/多模块依赖变更按 AGENTS.md 跑全套；否则专项足够。失败先区分真实回归与已记录环境，不删除用例。

退出还要求：类型/所有权/无反向依赖/零旁路结构证明；观察/输入/发布/凭据/退出矩阵；保留未验收平台范围。新测试成绩写入03实施与验收记录 与阶段状态，不以文件行数缩短证明正确。

## 低耦合、高内聚验收

- 职责边界：请求交付归Service；命令/执行阶段及预留归CommandRuntime管理，授权/观察/证据真源保持独立；拆用例不允许继续通过巨大Ports操作Service拥有的可变集合。
- 迁移接口：已用TaskId/不可变调用值替代跨模块shared_ptr<Job>、可变注册表与计数器；生命周期不允许只靠指针类型替换解决。
- [x] 列出每个现存Ports的字段消费者/owner，将与本用例无关的依赖移到组合根；独立用例测试不链接HTTP或Widgets，验证取消/迟到/未知结果仍由所属模块收口。
- [x] 复核公共头/CMake直接依赖和新增修改文件范围，防止业务状态拆散后形成原大类的隐式分布式实现。

## 原P8基础接口的延续

精确字段、annotations、错误和预算以[02当前契约](02_Current_MCP_Contract.md)及fixture为准，不能恢复档案里的五工具/stdio、旧整组grant或双提示符模型。
原P8.6输入仲裁/framing/协调/风险确认/脚本/产品界面映射至本阶段T2～T5核验：原文保存在04，按日期的实现证据保存在03。涉及无提示符SSH的既有限制如实保留，不用未来P1更严格的共享Recipe要求掩盖现行行为。

## P0验收证据索引与现存端口

| 清单 | 已核验测试/结构证据 |
| --- | --- |
| T1 | ContractsTests::catalogMatchesCompatibilityFixture、各工具默认值/可选字段/未知字段/枚举/UTF-8边界；WireSessionTests确认五种答复、一次完成、重复活动ID；endpoint两代断流/DELETE |
| T2 | AuthorizationTests绑定各维度、客户端行版本隔离和权限级联；McpTests旧行重连/旧客户端行事件；McpAccessInteractionTests凭据跨客户端撤权及异步存储 |
| T3 | AuthorizationTests::snapshotStoreCoalescesWithinWindow/snapshotStoreReloadsOnEpochChange/captureScopesKeepSeparateQuotas；ObservationStateTests；McpTests full/compact/UTF-8补读/注意等待；SessionTests提示符、用户代际、输入顺序及空批次 |
| T4 | CommandRuntimeTests取消/未知/迟到发布/owner销毁/同步完成/固定时钟过期/独立撤权；McpTests unknownExecutionProtectsTarget/privateMarkersSurviveServiceRestart/cancelledCommandNeverReplays |
| T5 | 三个纯应用测试动态依赖仅Qt Core/Test；compile_commands核对每个MCP生产cpp只有一个生产目标；Session源码无反向mcp include；runtime将HTTP/protocol列为PRIVATE |

| 现存依赖组 | 字段消费者及owner |
| --- | --- |
| AuthorizationPorts | 授权事务所需启用/客户端/版本/诊断/凭据操作事实；AccessStore/ClientRegistry拥有状态，AuthorizationService执行单项事务 |
| ReadUseCaseAdapter::Ports | access/sessions/observation由Service拥有，适配器只用于事实查询与投影；instance为只读输出值 |
| ReadSessionPort | 只读用例使用目录版本、授权版本、分页、上下文和诊断事实；具体Session只在CapabilityAdapter中 |
| CommandSessionPort | 命令用例查询授权/就绪事实并提交/取消命令或文件发布；具体对象、信号连接和通道ID仅归CommandSessionAdapter |
| CommandTargetStore | TargetReservation/CommandRuntime消费持久保护操作；具体私有文件及锁归适配器的TargetGuard |
| CommandRuntime::Delivery | complete/deadline/confirm/changed是类型化输出通知；不携带可变注册表或计数器，协议投影由适配器处理 |
| HTTP Backend/Terminal completion | 认证与协议请求路由、结果交付；不决定终端就绪、授权或真实执行终态 |

三个纯应用目标分别验证读取、观察状态和完整命令生命周期；没有仅把阶段枚举测试当作用例独立验证。私有shared_ptr仅用于同步回调/迟到发布保活，对外没有共享可变操作对象。
