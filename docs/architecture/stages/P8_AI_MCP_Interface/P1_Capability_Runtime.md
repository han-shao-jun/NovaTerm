# mcp/P1 能力目录、应用入口与执行路由实施计划

> For agentic workers：实施使用 superpowers:executing-plans；逐任务验证，复选框仅在证据成立后勾选。

**Goal:** 外部 MCP、本机 Agent 和未来 CLI 共享能力目录、可信身份、授权与执行用例，保留九工具兼容。
**Architecture:** Registry 提供稳定元数据，Dispatcher 接受类型化资源/请求，经授权和真实边界调用已有用例；不让 Runtime 依赖 HTTP/JSON-RPC。
**Tech Stack:** C++17、Qt Core、Qt Test、现有 Session 自动化。
**Spec:** [总体架构](00_Overall_Architecture.md)；依赖 [P0](P0_Runtime_Consolidation.md)。

## Global Constraints

不重写 HTTP、不开新终端、不升级旧权限、不新增 Transport 写旁路；完整输入维持既有免逐条确认，新增诊断权限独立。原诊断草案 QJsonObject Request/Context 只是语义示意，业务请求必须采用 typed variant，Schema 可在协议映射边界保存 JSON。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 模型自报 principal/clientId：T1/T2 拒绝，身份由入口生成。
- 排队/批准后撤权：T2/T3 提交前再验，零输入。
- U-Boot/Unknown 被当 Linux：T3 零 POSIX 探测；同连接环境切换失效。
- 共享诊断借用 FullTerminalInput：T3 独立 Recipe 授权，不扩大自由输入。
- Registry 和 Schema 漂移/反向依赖：T1/T4 fixture 和独立链接测试。

## 文件与接口

新建 src/capabilities/CapabilityTypes.h、CapabilityResult.h、CapabilityRegistry.h/.cpp、CapabilityDispatcher.h/.cpp、CapabilityAuthorization.h/.cpp、DiagnosticTaskManager.h/.cpp、CMakeLists.txt。
按消费者新增 src/capabilities/TargetEnvironment.h、SessionExecutionRouter.h/.cpp、McpCapabilityAdapter.h/.cpp、LocalAgentAdapter.h/.cpp；Session 具体适配保留在集成目标。
修改 src/mcp/McpService.*、protocol/ToolCodec.*、src/session/CommandPlatformProfile.*、根/测试 CMakeLists.txt。
新测试 tests/capabilities/CapabilityRuntimeTests.cpp，目标 novaterm_capabilities_tests。

规划公共语义接口（类型在 CapabilityTypes.h 定义，名称是本阶段拟定契约）：
- Definition：CapabilityId、版本、status、resource kind、policy、surface bindings；Schema 在 codec 映射。
- ResourceBinding：SessionBinding / HistoryCaptureBinding / DiagnosticResultBinding / ImportedFileBinding variant。
- InvocationContext：可信 PrincipalId、资源绑定、accessVersion、单调 deadline、OperationId。
- CapabilityRegistry::find(CapabilityId) const → optional definition 视图。
- 领域类型化端口invoke(Request, InvocationContext, Completion<Result>)；公共CapabilityDispatcher::admit管理准入租约，取消按OperationId，具体类型边界见文末补充契约。
- CapabilityAuthorization::validate(context, capability) → 授权快照或拒绝；MCP 用现有 AccessStore 适配，离线用资源授权适配。
- SessionExecutionRouter::resolve(TargetFacts, RecipeRequirements, AuthorizationSnapshot) → ExecutionRoute 或 unavailable reason，不自行探测/授权。

注册本身不创建任务/连接；仅对真正边界使用注入接口，不为每个 Handler 建立继承体系。

## T1：建立目录与兼容映射

**Consumes:** P0 ToolRequest/ToolCodec/Errors；**Produces:** stable capability definition 与 MCP 映射。
- [ ] 在 CapabilityRuntimeTests 加 catalogHasStableIds、unimplementedToolsNotRegistered、legacySchemasUnchanged；断言旧 fixture 对象相等、重复 ID 拒绝、未实现不发布。
- [ ] 配置新测试目标，运行并确认失败来自缺失 Registry/映射。
- [ ] 最小实现 Registry，先接 terminal.context.read，再接 session.list/context.search。MCP tool name/RPC ID 不充当内部能力 ID。
- [ ] 分离静态 tools/list 与逐目标 available/disabledReason；不随选中标签变动目录，不引入 Netcatty 式动态加载器。
- [ ] 验证 novaterm_capabilities_tests 与 MCP contracts/wire；记录目录版本和消费者。

## T2：可信 principal 与共用应用入口

**Consumes:** Registry、P0 授权/用例；**Produces:** MCP/本机适配可调用的同一 Dispatcher。
- [ ] 测 forgedPrincipalRejected、revokedBeforeDispatchDoesNotRun、sessionAndOfflineBindingsNotInterchangeable；断言 Handler 调用次数0。
- [ ] 测 readOnlyEntryParity：假 MCP 与本机入口在同绑定下得到同允许/拒绝和投影，动态字段规范化。
- [ ] 实现认证入口构造 Context，模型输入不可覆盖身份；排队和结果前校验版本。
- [ ] 先迁移读工具，再迁移执行工具；每次仅一条副作用提交路径，保留票据/elicitation/MRTR/unknown。
- [ ] 内置入口的开关/授权与外部服务开关明确分开，运行在相同下层规则之上。
- [ ] 删除迁移后的旧分派接缝，跑 capabilities + MCP 九项。

## T3：目标环境、路由与任务预算

**Consumes:** Session facts、授权快照；**Produces:** 不自动换路的 Route/不可用原因，任务容量与取消语义。
- [ ] 测 unknownEnvironmentSendsNoProbe、uartBootToLinuxInvalidatesRecipe、sharedDiagnosticDoesNotGrantFreeInput、backendFailureDoesNotRetryOtherRoute。
- [ ] 分离 TransportKind 与 TargetEnvironment；环境记录来源/revision，不将 Shell 标记当可信密码身份。
- [ ] 独立模式复用 SessionCommandFacade；共享模式走 Coordinator/Controller/Arbiter，不直接 Transport::write。
- [ ] 共享路径要求可见占用同意和就绪；TUI/REPL/登录/文件传输时零输入，不自动 Ctrl+C/退出。
- [ ] 引入诊断任务预算全局32/Principal4；每步提交前再验；出错/取消只终结一次，未知远端保留保护。
- [ ] 基础 text/key/paste 不因串口/无 Shell 禁用；command 快捷动作缺就绪拒绝。
- [ ] 跑 capabilities、Session/MCP 专项，五类连接矩阵使用隔离 fake，不连接真实设备。

## T4：构建与删除收口

**Consumes:** 已迁用例；**Produces:** 无 HTTP 反向依赖的 novaterm_capabilities 及适配器。
- [ ] 在不链接 HttpServer/Widgets/具体 Session 的小测试链接 Registry/授权/分派，具体 Session 仅由注入端口提供。
- [ ] 统一 Schema/工具映射消费者，去掉重复注册与旧 Handler 路由。
- [ ] 跑既有九目标，确认目录/确认/取消/输入去重不变。
- [ ] 更新阶段状态、实际依赖和未覆盖项；P6只读子集可按阶段划分§6.1提前消费已收口端口，P2/P6其余能力按对应闸门开放，不宣称P1全部完成。

~~~bash
cmake -S . -B build
cmake --build build --config Debug --target novaterm_capabilities_tests
ctest --test-dir build -C Debug -R novaterm_capabilities_tests --output-on-failure
ctest --test-dir build -C Debug -L mcp --output-on-failure
~~~
以上新目标在 T1 加入前不存在；实现时先按现有生成器/配置调整构建命令。退出：只读/执行适配同策略、无越权/旁路、独立链接通过。

## 低耦合、高内聚验收

- Registry管理目录，Dispatcher只做关联/分派；授权、任务、观察和执行规则由各自用例拥有，不能将它们的注册表全部传入Dispatcher。
- CapabilityTypes.h只容纳轻量ID/绑定/错误等稳定值，领域请求/结果与其模块共置；不建立随每个Provider扩大的全系统variant、std::any或JSON业务总线。
- Handler通过窄调用端口在组合根注册；能力核心不include诊断/历史/SFTP/Agent实现。Session观察、输入与独立执行按实际消费者给出小端口。
- [ ] 增加registryDoesNotDependOnConcreteHandlers构建验证：目录/分派测试仅链接契约与fake handler；加入一个测试能力时不修改Dispatcher业务分支。
- [ ] 记录认证/资源/执行路由的直接依赖与owner，拒绝循环依赖和依靠PUBLIC传递链接访问实现。

## 补充契约：领域类型化分派

本节替代前文CapabilityRequest/CapabilityResult可能被理解为全系统联合类型的解释；两者仅为各领域请求/结果的泛称，不创建同名中心数据集合。

选择“领域类型化调用端口 + 公共准入器”，不采用中心variant或任意类型容器。公共Dispatcher管理InvocationContext、OperationId、deadline、取消及准入租约；不知道领域payload。协议适配器与本机适配器通过同一个领域端口调用用例，端口内部必须经Dispatcher和授权校验，不能由调用者选择跳过。

| 示例能力 | 领域公开契约 | 消费者与注册 |
| --- | --- | --- |
| terminal.context.read | ContextReadPort::invoke(ReadContextRequest, InvocationContext, Completion<ObservationRead>) | MCP解码适配、本机工具适配消费；组合根绑定具体读取用例 |
| log.analyze（P5才注册） | LogAnalysisPort::invoke(LogAnalysisRequest, InvocationContext, Completion<AnalysisResult>) | 对应协议/本机适配消费；只依赖分析值契约，组合根绑定Analyzer |

端口将轻量CapabilityId、ResourceBinding和Context交给Dispatcher::admit，获得move-only InvocationLease或拒绝。租约绑定操作和授权版本，支持取消/到期查询与单次完成；副作用提交及结果交付仍调用授权复核，租约不是永久授权。任务owner持有租约，拒绝不创建业务任务，取消不代表远端已停止。Dispatcher仅保存有界取消端点/操作索引，不保存领域Job。

MCP和模型工具的JSON只存在于各能力的codec适配器；工具名称到codec的映射在组合根登记。codec只负责类型解码/编码，然后调用领域端口，不能包含策略或直接调用Transport。相同Schema由对应codec提供给各入口，不手写第二套。新增能力可新增领域值契约、端口、codec和组合根注册，不修改Dispatcher、既有领域类型或UI业务分支。

- [ ] T1/T2增加twoUnrelatedTypedPortsShareAdmission、deniedAdmissionNeverInvokesHandler、cancelLeaseDoesNotClaimRemoteStopped。
- [ ] T4用fake日志分析端口证明新增能力无需修改中心请求/结果集合；协议适配链接领域契约，Dispatcher测试不链接具体领域实现。

## 补充契约：环境事实与失效

EnvironmentTracker位于Session集成适配层，是环境事实唯一写入owner；P1仅通过只读端口消费快照。快照含kind、revision、connectionGeneration、来源、observedAt、validUntil和readinessEvidenceId。来源分为用户声明、已配对可信适配器、普通输出候选；环境声明不等于可信身份或输入就绪。

| 事件 | 环境转换与执行约束 |
| --- | --- |
| 新连接/重连 | revision递增，Unknown；旧声明/批准/Recipe不可复用 |
| 用户明确声明当前为Linux/POSIX等 | 建立本连接环境声明；仍需当前就绪证据与独立诊断授权，无证据则不可执行 |
| 已配对适配器报告环境与就绪 | 核验绑定和消息序号后更新快照；不得据此授予权限 |
| 普通日志/提示符出现Linux、U-Boot等文字 | 只产生候选提示；不能自动从Unknown升级可执行。与现有环境矛盾则失效 |
| 人工输入接管、reset、TUI/REPL/登录/传输边界、身份变化、适配器丢失 | revision递增并退回Unknown，撤销未提交Recipe与批准；已提交任务按unknown/终止证据收口 |
| 证据超时或无法确定是否发生切换 | 同上；禁止复用旧revision继续发送POSIX命令 |

首版环境/就绪证据有效期上限5秒（单调时间，设计初值），每次共享Recipe提交要求处于该期限且无后续失效事件；每步重新校验。有效期不能证明期间环境绝未变化。没有可信适配器的共享路由要求用户对本次计划明确确认当前环境与就绪，确认最多授权一次提交，5秒过期；多步操作逐步确认，不因匹配普通提示符自动续租。无法接受此交互成本的目标暂报unavailable，不以启发式替代可信就绪。

同一连接上无可观测信号的远端切换无法可靠检测；文档和UI必须保留这一限制。可信适配器存在时仍需在提交前验证最新绑定。此约束只适用于新增Recipe共享路由，不暗改现行九工具的输入语义。

- [ ] T3增加plainBootTextCannotAuthorizeLinux、unobservedSwitchCannotReuseExpiredEvidence、manualInputInvalidatesRecipe、confirmationAllowsOneSubmission、oldAdapterSequenceRejected。
- [ ] P4/P5共享路由复用以上事实；没有适配器/一次性本机确认的fixture必须断言零输入。
