# mcp/P4 Backend 诊断实施计划

> For agentic workers：实施使用 superpowers:executing-plans；按能力逐个交付，不并行改写同一 Service。

**Goal:** 在获准 Linux 目标上用可信、结构化、只读 Recipe 收集系统/服务/日志/进程/网络/容器诊断，并返回可追溯证据。
**Architecture:** Provider→Profile/RecipeResolver→独立或共享 Session 路由→有界任务结果→P3 EvidenceStore。
**Tech Stack:** C++17、Qt Core、现有 Session executor/coordinator、Qt Test。
**Spec:** [总体架构](00_Overall_Architecture.md)；依赖 [P1](P1_Capability_Runtime.md)、[P3 证据契约](P3_History_Search_and_Evidence.md)。

## Global Constraints

不 restart/kill/docker exec/改防火墙/部署配置；不自动 sudo、安装工具或写临时文件。Recipe 参数不是自由 shell，Profile匹配不证明可信目标身份。独立执行不继承当前shell环境；共享执行先声明可见终端占用，使用专用有限诊断授权而非要求 FullTerminalInput。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- Unit/接口/容器名携带引号、换行、路径注入：T1/T2 严格字段Schema/编码。
- 非systemd或精简BusyBox缺工具：T2 unavailable，不自动安装或用错误Recipe。
- 共享终端正在TUI/REPL/登录/传输：T3 零输入，不自动Ctrl+C。
- 提交后断线/授权撤销：T3 unknown且无跨路重试。
- 未开启录制时证据：T3 保留期限内存结果，不伪造历史持久化。

## 文件与接口

新增 src/diagnostics/common/IDiagnosticProvider.h、DiagnosticRecipe.h、TargetProfile.h、DiagnosticResult.h、DiagnosticRecipeResolver.*、CMakeLists.txt。
新增 src/diagnostics/backend/LinuxSystemProvider.*、SystemdProvider.*、JournalProvider.*、ProcessProvider.*、NetworkProvider.*、ContainerProvider.*、CMakeLists.txt。
修改 src/session/CommandPlatformProfile.*、P1 Registry/路由、src/mcp/CommandPolicy.* 与 MCP adapter；Service只委托，不内联 Provider 分支。
测试 tests/diagnostics/backend/BackendDiagnosticsTests.cpp、RecipeSafetyTests.cpp；目标 novaterm_backend_diagnostics_tests。

契约：
- TargetProfile：id、architecture、initSystem、availableTools、diagnosticIds、version、environment/revision、executionModes。
- DiagnosticRecipe：id/profileId、executable、受限argumentTemplate/schema、allowedExecutionModes、timeout默认5000ms、maxOutputBytes默认65536。
- IDiagnosticProvider::list(TargetProfile, AuthorizationView) → 可用诊断/原因；run(DiagnosticRequest, InvocationContext, Completion)→DiagnosticResult。
- DiagnosticRequest 接收 diagnosticId 和类型化受限字段，禁止任意参数透传、模型直接拼 shell 或传 sh -c。
- DiagnosticResult：结构化采集事实、证据、执行模式/版本、截断/退出或unknown、limitations。

拟新增 novaterm_list_diagnostics、novaterm_run_diagnostic；P5复用这些入口，通过diagnosticId分派，不每Provider增加MCP工具。

| Capability ID | 初始来源与意图 |
| --- | --- |
| system.inspect | /proc、uname、df：OS/CPU/内存/负载/磁盘 |
| service.inspect | systemctl show：服务状态/失败原因 |
| journal.query | journalctl：Unit/时间/级别有界查询 |
| process.inspect | /proc、ps：进程/线程/资源 |
| network.inspect | ss、ip、/proc/net：监听/路由/接口 |
| container.inspect | Docker/Podman只读接口：状态/健康 |

## T1：Recipe 与 Profile

**Consumes:** P1环境/授权/路由；**Produces:** 审核过的结构化命令计划。
- [ ] 测 maliciousUnitRejected、newlineArgumentCannotAppendCommand、profileMatchDoesNotGrantPermission、environmentRevisionInvalidatesRecipe。
- [ ] 固定每能力参数集合、长度/字符/语义和输出/超时；可变字段交给受审计编码器，禁止argv自由尾部。
- [ ] 显式禁用 systemctl/journalctl pager；命令执行前校验工具/路由，已发现工具只影响可用性。
- [ ] 只读表示审核过的采集意图，不声称恶意目标上的程序/插件天然无副作用。

## T2：Provider 逐能力实现

**Consumes:** RecipeResolver；**Produces:** 结构化事实/limitations，不让LLM替代解析器。
- [ ] 写 system缺proc、service active/failed/inactive/not-found/denied、journal缺失、IPv4/IPv6/缺ss或ip、Docker无安装/daemon不通/exited/unhealthy夹具。
- [ ] 确认失败来自缺失Provider或转换逻辑，再依次实现System→Service/Journal→Process/Network→Container。
- [ ] 所有输出有界；非零退出、工具运行故障和截断分别表示，不将空输出一律当成功。
- [ ] 未配置/缺工具返回structured unavailable，不隐式改用其他Recipe或安装。

## T3：执行、结果与证据

**Consumes:** DiagnosticRequest/Context；**Produces:** 有证据与完成状态的结果。
- [ ] 测 sshIndependentAndSerialSharedUseSameRecipe、busySharedTerminalSendsNothing、revokedBeforeNextStepStops、unknownNeverRetriesOtherRoute。
- [ ] 独立复用SessionCommandFacade；共享复用Controller/Coordinator/Arbiter，不通过更高权限公开interact接口借权。
- [ ] 每步和结果交付重验，明确 shared_terminal 来源，不声称独立stdout归属。
- [ ] 录制关闭时使用P3期限内存证据，持久化/跨客户端共享另授权；不得偷偷录制。
- [ ] end-to-end夹具模拟nginx启动失败：systemd+journal+可选网络，输出结论/证据/限制，不修改服务。

## T4：目录与安全验收

**Consumes:** 已实现Provider；**Produces:** 两个新MCP工具和逐目标能力目录。
- [ ] list_diagnostics区分可见/授权/就绪原因，稳定Schema，保持九工具fixture。
- [ ] 在SSH和显式Linux串口共享两路分别验收，不能以SSH成绩代替通用性。
- [ ] 运行Backend、capabilities、MCP专项；真实主机部署/连接另需授权。

~~~bash
cmake --build build --config Debug --target novaterm_backend_diagnostics_tests
ctest --test-dir build -C Debug -R novaterm_backend_diagnostics_tests --output-on-failure
ctest --test-dir build -C Debug -L mcp --output-on-failure
~~~
退出：六类能力有fixture、注入/撤权/unknown/双路证据，缺环境/工具/权限原因明确。在线目标支持范围按实际验证报告。

## 低耦合、高内聚验收

- Provider内聚自己的Recipe、参数约束和结构化结果；共享的执行/证据仅通过窄端口消费，不把Provider分支写回Dispatcher或McpService。
- 诊断核心不includeTransport、HTTP适配、HistoryChunkStore或UI；独立/共享路由由P1边界负责，Provider不绕过执行仲裁。
- [ ] 用fake执行结果与证据发布端口独立链接Provider测试；新增一个Provider仅增本模块、组合根注册和对应fixture，不改既有UI/分派业务。
- [ ] 保留同一授权/就绪真源，Provider不得缓存可写grant或复制Session证据状态。

## 补充实施约束（设计补齐，尚未验收）

- T1/T3消费[P1环境事实与失效契约](P1_Capability_Runtime.md#补充契约环境事实与失效)：Recipe绑定environment revision、就绪证据和有效期；每步重新验证。无可信适配器的共享路由使用本次计划的一次提交确认，多步逐步确认；普通输出不能续租。新增测试sharedRecipeWithoutFreshEvidenceSendsNothing和expiredEnvironmentStopsNextStep。
- 首个诊断闭环可用现有固定诊断；本阶段新增Provider只在P1执行边界和P3内存Evidence服务可消费后接入，不强制等待完整历史UI。
- 结果与报告携带来源完整性；有unknown_tail或扫描不完整时只能报告已观察范围。交给Agent的结果继续受P6模型发送授权约束，诊断权限不授予模型发送权限。
