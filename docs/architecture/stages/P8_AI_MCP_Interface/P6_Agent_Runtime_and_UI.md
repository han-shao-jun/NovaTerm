# mcp/P6 内置 Agent 运行时与聊天 UI 实施计划

> For agentic workers：实施使用 superpowers:executing-plans；按先只读、再受控执行、后长对话顺序验证。

**Goal:** 侧栏实现模型/工具/观察闭环，用户看见目标、批准动作、随时停止接管，聊天可恢复。
**Architecture:** 应用级AgentManager拥有Runtime；Driver产生统一事件，工具适配P1共享入口；上下文/输出归档独立于终端观察和Raw History；UI只消费消息模型和提交意图。
**Tech Stack:** C++17、Qt6.8.3 Network/Widgets、ElaWidgetTools、Qt Test；首版一种OpenAI-compatible Chat Completions协议。
**Spec:** [总体架构](00_Overall_Architecture.md)，Netcatty本地源码调研；基础依赖[P1](P1_Capability_Runtime.md)，可选历史/诊断集成依赖P3～P5。

## Global Constraints

内置Agent不绕HTTP导出Bearer、不复制Session/授权内核，不要求开启外部MCP。模式只收紧底层授权。首版不自动开主机、不升级C++、不一次实现全部Provider。
NovaTerm命令操作当前shell，cd/export状态持久语义与Netcatty隔离子shell不同，系统提示/工具描述/卡片必须一致。

所有模块必须遵守[总体架构](00_Overall_Architecture.md)§2.1～§2.4的低耦合、高内聚规则；下列专项结构验收是本阶段退出条件。

## Review Focus

- 参数尚未组装完整已执行：T2任意SSE/JSON分片下完成前零调用。
- Confirm显示批准却text/key直通：T3没有对应边界不暴露任意输入。
- 413/断流后重发副作用：T4保留已提交事实，不换operationId自动重试。
- 关侧栏/换标签丢任务或串目标：T1/T5应用owner和绑定快照。
- 重启/fork/撤销重放操作：T5只恢复事实，不声称远端回滚。

## 文件与接口

新增 src/agent/contracts/AgentIdentifiers.h、AgentEvent.h、AgentTurn.h、AgentMessage.h、DriverCapabilities.h；
src/agent/AgentManager.*、AgentRuntime.*、AgentToolAdapter.*、AgentPolicy.*、ContextManager.*、SessionStateStore.*、OutputArtifactStore.*、ChatSessionStore.*；
src/agent/drivers/ModelDriver.h、OpenAIChatDriver.*、ProviderConfig.h；
src/ui/ai/AgentPanel.*、MessageModel.*、ChatComposer.*、ToolActivityCard.*、ApprovalCard.*、AgentSettingsPanel.*。
修改应用组合根、侧栏、凭据适配和根/测试CMake；不向Core加入模型类型。
测试 tests/agent/AgentRuntimeTests.cpp、ModelStreamTests.cpp、AgentPolicyTests.cpp、ContextManagerTests.cpp、ChatRecoveryTests.cpp、tests/ui/AgentPanelTests.cpp。
新增目标 novaterm_agent_runtime_tests、novaterm_agent_ui_tests；前者不链接Widgets，后者offscreen。

公共契约均为规划：
- TurnInput：ChatSessionId、AgentTurnId、后端配置快照、用户文本/附件、scope绑定、deadline/cancel。
- AgentRuntime::runTurn(TurnInput)→异步TurnResult；同chat活动返回AGENT_TURN_BUSY。stopTurn(ChatSessionId,StopReason)唯一停止入口；waitForTurn用于替换历史。
- TurnResult：completed/aborted/error，不代替工具ExecutionOutcome/退出码。
- TurnDriver::run(TurnInput,TurnContext)；Context给emit/取消/工具端口，不给QWidget回调。
- DriverCapabilities：tools/images/reasoning/usage/modelList/resume/steer/nativeApproval/nativeQuestions。
- AgentEvent：version、event/chat/turn ID、sequence、timestamp、backend、typed payload；关联ToolCallId/ExecutionId。
- AgentToolAdapter::invoke(ToolInvocation,TrustedAgentContext,Completion)→ToolOutcome；身份/权限非模型参数。
- ToolOutcome区分策略拒绝、工具故障、非零退出、取消、unknown、截断与artifact引用。
- ChatSessionStore保存scope、消息/附件元数据、活动/usage、压缩边界、backendIdentity和时间；草稿默认只存内存。
- OutputArtifactStore::store/read/prune拥有owner、目标绑定、配额/TTL/失效原因，不用60秒观察capture作永久归档。

事件包含turn_start/end、model/reasoning_delta、tool_call/result、approval_requested/resolved、plan/file/search、context_snapshot、compaction、usage/performance、error。
状态为preparing/streaming/awaiting_approval/running_tool/compacting/stopping和终态；同chat互斥、不同chat争用终端仍经Arbiter。

## T1：运行时与事件

**Consumes:** P1本机应用入口；**Produces:** 头less Runtime和MessageModel事件输入。
- [ ] 测 sameChatRejectsConcurrentTurn、differentChatsStillUseTerminalArbiter、lateEventAfterStopDoesNotMutateNewTurn、eventSequencePreserved。
- [ ] 注册测试并验证缺实现失败，再实现应用owner、per-chat活动、配置/目标快照和唯一stop。
- [ ] 正文增量有界合并，工具/批准/终态有序交付；订阅者生命周期安全，结束只发一次。
- [ ] FakeDriver提供文本/工具/失败/取消，不需要真实账号跑状态机。

## T2：模型协议和工具循环

**Consumes:** TurnDriver/Event/ToolAdapter；**Produces:** 一种可取消的模型→工具→模型闭环。
- [ ] 测 everyByteSplitSseReconstructsCall、multipleToolCallsRemainDistinct、partialArgumentsNeverExecute、idleTimeoutCancelsRequest。
- [ ] 异步Qt网络组装SSE与工具参数；行/frame/参数/输出有上限，组装未完成零执行，错误在边界收口。
- [ ] Provider identity与协议family分开；Chat Completions不自动等同Responses，后续Anthropic/Google/Responses分别适配。
- [ ] 无真实usage时标估计，模型目录说明实时/缓存/预设；API key/自定义头由凭据边界存取，不进入导出/日志。
- [ ] step默认20、网络idle默认120秒为规划；工具等待沿用下层预算，不把30秒等待超时解释成远端结束。
- [ ] 模型本机工具请求只能调用注册产品能力，不自行建立SSH连接/取密。

## T3：模式、批准与执行

**Consumes:** P1授权/确认/绑定；**Produces:** 可审阅批准卡片和真实执行结果。
- [ ] 测 observerCannotExecutePwd、confirmCannotBypassThroughTextAction、approvalRejectSendsZeroBytes、reconnectInvalidatesPendingApproval、autoCannotGrantNewScope。
- [ ] Observer仅读；Confirm给暴露写/执行动作真实本机确认；Auto免聊天层提示但不绕过下层各能力已有确认、授权或永久拒绝。
- [ ] 首版优先固定诊断/受控命令；任意text/key/paste未有对应本机批准策略前不提供给Confirm。完整输入由用户明确选择，不能伪装Confirm。
- [ ] 卡片显示目标、执行模式、命令/参数、理由/有效期；可信UI答复，模型不能自报approved。
- [ ] 复用operationId/ticket/inputToken/replyToExecutionId；未知提交不换ID重发。停止与人工接管阻止后续步骤。
- [ ] 分开工具运行故障与命令非零退出，保留submitted/完成/unknown证据；终端输出、附件和标题始终视为数据。

## T4：上下文与输出归档

**Consumes:** messages/tool outcomes；**Produces:** 可预算/压缩/恢复的模型上下文。
- [ ] 测 compactionKeepsToolPairs、reinjectsScopeGoalAndPermissions、expiredHandleDoesNotRerunCommand、requestTooLargeAfterToolDoesNotExecuteTwice。
- [ ] 保留工具配对和provider continuation；预算含系统/工具/历史/输出/推理预留，不按条数粗裁。
- [ ] 回合前可摘要；step做类型化裁剪；413/手动压缩独立恢复，不能重复副作用。
- [ ] 工作记忆保存目标/计划/活动执行/读取游标/修改/阻塞，不替代授权或终端真源。
- [ ] 输出预览+handle，按head/tail/range/search读取原文，不重执行补原文。
- [ ] ArtifactPolicy规划为UTF-8 bytes：单读12KiB、单artifact4MiB、chat64个/8MiB、全局256个/32MiB、TTL30分钟。区别于Netcatty的JS字符计量，不是既有MCP预算。
- [ ] 持久输出需要明确本机策略/授权；不可用时仅有界内存，说明重启不可恢复。

## T5：Ela侧栏与聊天恢复

**Consumes:** Runtime事件/MessageModel；**Produces:** 可操作的完整交互闭环。
- [ ] 测 hiddenPanelKeepsTurn、switchTabDoesNotRetargetTool、pendingApprovalReappears、historyLoadNeverInvokesTools、undoDoesNotClaimRemoteRollback。
- [ ] 头部Agent/模型/权限/历史/新聊天；消息正文/推理折叠/工具组/批准/错误；输入多行/选区/附件/停止/预算。
- [ ] 优先Ela控件、主题和MessagePrompts；不覆写Ela QSS/objectName，UI槽不阻塞网络/磁盘。
- [ ] 长消息按需分批呈现，隐藏时保留输入与Runtime、减少渲染；复制/展开/滚动控件不得触发批准。
- [ ] 保存活动/usage/压缩边界；重启未完成卡片标interrupted/unknown，重新观察而非重放。
- [ ] 先文本+选区+只读，再批准执行；fork/撤销只回退对话，导出隐藏秘密字段。
- [ ] P3～P5未实现不显示相关工具，实现后接同一目录，不另建聊天专属诊断。

## 验证与退出

~~~bash
cmake --build build --config Debug --target novaterm_agent_runtime_tests novaterm_agent_ui_tests
ctest --test-dir build -C Debug -R "novaterm_agent_(runtime|ui)_tests" --output-on-failure
ctest --test-dir build -C Debug -L mcp --output-on-failure
~~~
新目标加入后运行，默认FakeDriver/回环fixture，无真实API key/付费模型。退出：选区提问→观察→批准诊断→执行结果→停止/接管，切标签/隐藏/重启不串目标和重发。

## 低耦合、高内聚验收

- Runtime内聚回合状态/编排，ContextManager内聚预算/压缩，ChatSessionStore内聚会话持久化，ArtifactStore内聚输出生命周期；不把它们的全部容器塞进AgentManager。
- TurnDriver表示顶层后端（内置/外部）运行契约；单次模型协议ModelDriver只处理请求/分片/错误，不执行产品工具、不申请权限、不反向持有Manager或UI。内置后端编排模型与工具闭环，外部后端适配其原生运行时。
- 工具适配仅依赖P1目录/调用和可信上下文端口，不include具体诊断/History/SFTP实现；UI通过命令、只读快照和有序事件观察应用服务。
- [ ] ModelStreamTests单独使用fake网络验证协议，Runtime测试使用FakeDriver/FakeToolPort，无HTTP服务、真实Session或Widgets；新增模型后端不改工具政策与消息UI分支。
- [ ] 隐藏/替换UI不改变回合owner；更换聊天存储不改Driver；事件用于展示事实，不用全局事件总线隐藏执行/取消依赖。

## 补充契约：模型发送与派生内容授权

本机读取、模型发送和持久保存是三个独立决定。用户启用只读Agent时明确选择允许的目标/内容范围及模型接收方；Observer只限制工具副作用，不意味着内容不会离开本机。用户新输入或显式选区发送可构成本次内容的发送意图，不能隐式覆盖其他资源、旧聊天或自动采集内容。

新增ModelRecipientBinding（规范化endpoint origin/base path、provider配置ID/版本、账户引用身份、协议family；不含secret）、ModelEgressGrant（Principal、recipient、资源/范围、授权版本、期限）和ContentProvenance（来源ID/代际/范围、保留策略、派生依赖）。配置快照绑定回合，切换endpoint/账户或后端身份使旧发送许可失效；模型名称在同一绑定内变更遵循用户已选范围，不凭同名provider认定同接收方。

ModelEgressPolicy置于Agent应用层，依赖窄资源授权与接收方策略端口；Driver只接受经校验的有界发送快照。每次模型请求（包括摘要模型请求、重试、工具返回后的下一步）在网络提交前校验来源读取/再次使用权限和发送许可。校验与网络请求接纳在同一串行owner完成；跨线程只投递绑定授权版本的一次发送票据，网络提交前消费并拒绝已撤销/过期票据，不能仅在组装上下文时检查。默认只允许已配置接收方，禁跨origin自动重定向和自动备用provider；变更接收方要求新的明确发送意图，凭据头不随重定向转发。

| 内容状态 | 本机显示/持久化 | 下一次模型请求 |
| --- | --- | --- |
| 来源仍获准、接收方获准 | 按独立保留策略 | 可纳入范围内内容 |
| Session撤权或资源删除/到期 | 已有副本仅按此前明确保留策略显示，否则遮蔽/清除；不承诺撤回远端已收到的数据 | 默认禁止再次发送，包括相关工具输出、摘要与artifact |
| 显式持久共享的离线副本 | 作为独立资源按自身授权/保留策略处理 | 必须核验该副本的独立读取与发送许可，不能继续借原Session许可 |
| 切换模型接收方 | 本机历史可按原保留策略显示 | 旧许可不适用；选择新接收方和范围后重建上下文 |

摘要、模型回复、计划和工作记忆继承本次输入依赖的并集；不能以“模型生成”去掉来源约束。首版保守地在依赖不再允许时移除整个派生项，显示内容不可用占位，并维持tool call/result协议配对；不能安全裁剪则开启新上下文。来源集合有界，超过1024项时拒绝继续自动扩展上下文并要求收窄范围，不丢弃来源标签换取成功。

ChatSessionStore与OutputArtifactStore同用保留策略：消息中的完整工具正文不能绕过artifact的TTL/配额，内存来源的派生摘要也不能默认永久落盘。恢复时重新校验授权、接收方及来源版本；未知来源的旧消息默认仅可按本机保留规则展示，不自动送模型。已在网络提交的请求遇撤权立即取消并禁止后续请求，明确无法撤回已发送字节。

- [ ] T1/T4增加readGrantDoesNotGrantModelEgress、recipientSwitchRequiresNewIntent、revokedSourceExcludedFromNextRequest、summaryInheritsSourceRestrictions、artifactTtlCannotBeBypassedByChatBody。
- [ ] T2增加crossOriginRedirectNeverForwardsPayloadOrCredential、revocationBeforeNetworkSubmitSendsNothing；T5增加restoredChatRechecksRecipientAndSources。
- [ ] 测试使用fake网络并检查实际请求体/发送次数，不能只断言工具没有再执行。P7外部Driver同样适用；不能证明原生resume不会重带旧上下文的后端禁用该resume，另开干净会话。
