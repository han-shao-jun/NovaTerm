# MCP、诊断与 AI Agent 总体架构

日期：2026-10-18。适用：C++17 / Qt 6.8.3。阶段入口：[阶段划分](01_Phases_and_Acceptance.md)。

## 1. 目标与兼容边界

在现有 Session 和进程内 Streamable HTTP 上，形成可由外部 MCP、内置 Agent、未来 CLI 复用的能力应用内核；再增加保真历史、证据、Linux/嵌入式诊断和聊天交互。不得为新入口复制授权、输入泵、会话目录或执行真源。

兼容基线是当前九工具及 tests/mcp/fixtures/tool-catalog-2026-10-18.json。名称、input/output Schema、默认值、enum、可选性、未知字段拒绝、错误码、retryable、封装、预算、确认/取消语义保持不变。动态 ID、时间、随机签名不要求逐字相同。新增工具是后续版本扩展，不与内部迁移混做。

| 当前工具 | 语义 |
| --- | --- |
| novaterm_list_sessions | 获准会话及能力视图 |
| novaterm_read_context | 有界屏幕/摘要，full/compact、等待与展开 |
| novaterm_search_context | 搜索已返回 capture，不是完整历史 |
| novaterm_list_commands | 固定诊断目录和短期票据 |
| novaterm_execute_command | 按票据运行固定诊断 |
| novaterm_run_command | 当前终端风险受控命令 |
| novaterm_run_script | 明确路径发布脚本后在当前终端调用 |
| novaterm_interact_terminal | 完整输入授权下的 text/key/paste/command/release |
| novaterm_request_elevation | 独立提权请求；可信适配器缺失时不可用，零取密/零输入 |

保持 loopback + Bearer、默认 http://127.0.0.1:38973/mcp、环境变量 NOVATERM_MCP_HTTP_PORT、配置导出和端口冲突报错。MCP 默认关闭，不恢复独立 stdio/IPC 桥。
2025-11-25 保留 initialize、MCP-Session-Id、SSE、Form Elicitation；普通 TCP 断流不等于取消，DELETE 结束会话。
2026-07-28 按请求 metadata/MRTR，无协议 session；请求流断开取消该请求。跨请求确认仍绑定有效 Bearer 上下文。

非目标：本轮文档不授权功能实施、真实 UART/SSH 访问、自动登录/开终端、安装 hook/helper、真实认证适配器、MMIO/JTAG、服务修改或固件刷写；不升级 C++、不恢复暂停的 Core 去 Qt/匹配器重构。

## 2. 当前事实与目标分层

已有：contracts/ToolCodec、单项授权、CapabilityResolver、统一观察存储、请求调度、统一确认事务、CommandRuntime执行生命周期、Session证据模型及独立只读用例。构建目标以 src/mcp/CMakeLists.txt 为准。
未有：通用能力 Registry/Dispatcher、本机 Agent 入口、保真历史、诊断 Pack、模型 Driver、完整聊天 UI、外部 Agent 托管。

~~~mermaid
flowchart TB
    MCP[现有 HTTP / WireSession / ToolCodec] --> Entry[共享应用入口与能力分派：待收敛]
    Agent[内置 Agent Runtime：规划] --> Local[本机工具适配]
    CLI[未来 CLI / 外部托管适配] --> Entry
    Local --> Entry
    Entry --> Auth[授权 / 能力解析 / 确认 / 审计]
    Entry --> Observe[统一观察服务]
    Entry --> Exec[任务与执行用例]
    Entry --> History[历史 / 搜索 / 证据：规划]
    Entry --> Packs[诊断 Provider：规划]
    Packs --> Exec
    Packs --> History
    Observe --> Gateway[Session 适配边界]
    Exec --> Gateway
    Gateway --> Session[Session / EvidenceModel / Arbiter / Providers]
    Session --> Transport[既有 Transport 与输入泵]
~~~

依赖规则：
- wire/HTTP 只做认证路由、报文与协议交付；不直接决定终端空闲、资源授权或诊断路由。
- 应用内核不依赖 HTTP/JSON-RPC、QWidget 或具体 Transport；JSON 保留在 codec、模型/协议、摘要和持久化边界。
- Session 不反向依赖 MCP；SessionGateway 具体适配器位于集成层，不放 Session 目录。
- Qt Core 值类型可用于应用层，不向纯 Core 数据类型新增 Qt 依赖；跨线程元类型集中在桥接层。
- 一文件仅由一个生产目标编译；不为目录整齐搬动既有 Framer/Arbiter/Provider，不建立无消费者空壳库。
- 注入仅针对时钟、Session、存储/凭据等真实边界；不建立服务定位器、全局单例或每类一接口的工厂树。

### 2.1 低耦合、高内聚是强制架构约束

模块围绕一组共同不变量、状态生命周期和变更原因组织；“读取/授权/执行/历史/证据/诊断/Agent/UI”各自拥有业务规则和资源。不能仅把大类拆成若干文件、回调或静态库便认定完成解耦。

低耦合通过单向依赖、最小公开契约、适配器和可替换的真实边界实现；高内聚通过把状态、约束它的操作、失败与回收逻辑放在同一模块实现。相近代码优先留在同一具体类，只有独立职责/替换需求才抽接口，避免把一项用例拆成多层无意义转发。

上图表示运行期调用关系，不等于编译依赖。下表规定目标编译边界，现有不满足项作为P0/P1迁移任务，不能靠传递链接掩盖。

| 模块 | 对外最小契约与允许依赖 | 禁止依赖/持有 |
| --- | --- | --- |
| 值契约 | 具名ID、绑定、错误、预算、不可变结果；只依赖所需标准库/Qt Core | UI、网络协议、存储实现、Session指针或全系统模型集合 |
| 协议/入口适配 | 对应协议codec + 应用用例端口 | 业务可变表、直接Transport写入、Provider具体实现 |
| 授权 | principal/资源事实→决策和失效事件；只读事实端口 | UI控件、模型后端、实际终端I/O |
| 能力目录/分派 | 元数据、查找、调用端口和关联ID | 诊断/存储/模型实现头、所有模块注册表或业务状态 |
| 观察/执行应用 | 各自类型契约 + 最小授权/Session端口 | HTTP/MCP报文、Widget、Driver或其他用例内部状态 |
| Session自动化 | 自身证据/输入仲裁/Transport能力 | MCP、Agent、History存储或诊断Provider |
| History录制/存储 | 接收流提交、已提交记录读取、配额/回收 | Evidence实现、诊断结果类型、Agent消息 |
| Evidence/导入资源 | 证据引用、资源授权、按来源注册的只读解析端口 | History/诊断/Agent的具体存储和任务实现 |
| 离线Analyzer | 分段LogInput、选项→Finding/原文范围 | Session、网络、HTTP、Widget、磁盘格式 |
| 在线诊断 | Profile/Recipe、执行端口、证据发布端口 | Transport、HTTP适配、HistoryChunkStore、聊天UI |
| Agent运行时/Driver | 回合事件、模型协议与工具调用端口；Driver不反向依赖Manager | 具体诊断/历史/SFTP实现、UI对象或可变授权存储 |
| UI | 展示快照、应用命令/查询和事件投影 | worker/网络/执行器、可变业务注册表及直接权限判定 |
| 组合根 | 创建具体实现、注入端口、注册handler、连接生命周期 | 内联命令策略、脚本状态机或运行期通用服务查找 |

共用值类型保持小且稳定，领域专有请求/结果与本模块共置；不建立包含所有Provider、任务、消息和数据对象的Common/Types大集合。P1的资源绑定variant仅组合轻量值契约，不拉入对应模块实现。领域事实在边界转换，不让MCP类型成为Session/History/Agent的通用语言。

### 2.2 状态、接口与交互规则

- 一个可变聚合一个写入所有者；其不变量、提交、失败、取消、到期、回收在同一职责边界收口。其他模块持ID、不可变快照或窄查询端口，不能修改内部容器。
- 跨模块调用采用显式命令/查询；事件发布已经发生的事实并供投影消费，不通过全局事件总线隐蔽调用链。取消/撤权等安全动作必须有明确处理路径和完成语义。
- 构造注入按用例提供最少依赖。禁止传整个Service/Manager/Runtime、共享可变Context包、多个注册表/计数器或大量回调，让新类替原大类操作外部状态。现有巨大Ports属于待收口接缝，不作为新模块模板。
- 可共享的只读授权事实和不可变基础快照可以复用；共享可变Job、输入状态或权限缓存不能用于省略接口设计。观察投影不得反向写入事实源。
- 公开头仅暴露本职责需要的值类型和端口；实现头/第三方SDK/协议结构留在适配器或cpp。CMake公开依赖仅包含公共契约真正需要的目标，不依赖传递链接获得实现库。
- 生命周期与线程亲和由owner声明，异步边界传有界值/不可变引用。必要的任务邮箱/取消状态有明确范围，不将整个可变任务共享到各子系统。
- 可观测性通过脱敏值事件/只读统计接入，不让业务服务为日志/性能UI反向依赖观测实现。

### 2.3 组合与扩展边界

共享的是安全契约和用例能力，不是把所有模块合并到一个“能力超级内核”。Dispatcher只查找/关联/分派，业务状态留在所属模块；Registry元数据与handler装配分开，具体Provider只在组合根注册。SessionGateway按观察、输入、独立执行等实际消费者拆小端口，不形成暴露完整Session的万能门面。

Evidence作为独立应用职责：History、诊断、导入分别提供来源适配器，组合根向Evidence注册；Evidence核心只见原文读取/授权契约，不include它们的存储或任务实现，来源模块也不依赖Evidence内部索引。这样避免History→Evidence→诊断→History循环。

新增模型后端通过Driver扩展，新增诊断通过Provider/Analyzer扩展，新增存储通过读取/写入边界扩展；这些常规扩展不要求修改Dispatcher业务分支、已有UI分支或其他领域模型。只有公共语义变化才演进契约，并由独立兼容测试证明。

### 2.4 结构验收

每次增加跨模块依赖都记录消费者、最小契约、owner、依赖方向和验证方式。阶段完成必须满足：
- 无头文件/链接依赖环，无反向依赖，不依靠仅拆文件隐藏相同的可变状态耦合。
- 核心用例可用假端口独立链接/运行，不要求启动GUI、HTTP服务、真实Session或模型进程。
- 为普通扩展给出变更范围：更换模型后端不改工具/授权规则；更换存储不改Analyzer；新增Analyzer不改MCP/聊天UI；更换入口不改业务用例。
- 失败/取消/销毁测试能在所属模块验证资源和状态收口；不存在多个模块共同维护同一状态的合法组合。
- 类/库/接口数量及文件行数不是完成指标；发现巨大Ports、通用服务包、重复状态或循环依赖应回到职责边界修正，不继续加一层包装。

## 3. 核心职责与所有权

| 组件 | 唯一责任与边界 |
| --- | --- |
| Service / 集成组合根 | 生命周期、组件装配、入口接线；不重新内联九工具业务 |
| ToolCodec / 协议适配 | Schema、类型化解码、错误/结果编码；不拥有授权/Session |
| CapabilityRegistry / Dispatcher（P1 已实现） | 版本化能力元数据、类型化请求分派；不复制各用例状态 |
| AuthorizationService / GrantStore | 单项授权事务、绑定/版本、级联与失效 |
| CapabilityResolver | 授权 + 当前事实 → 是否可执行及明确原因；不探测、不授予 |
| ObservationService | 一个基础快照源、CaptureStore、读取/搜索；不执行命令 |
| RequestScheduler | 请求名额、排队、取消/交付；不决定远端是否结束 |
| Service请求表 / CommandRuntime | 请求由Service唯一持有；命令状态、阶段、发布与取消由CommandRuntime拥有，跨组件仅传ID和调用值 |
| ConfirmationRegistry/业务事务 | 摘要、目标/策略/代际绑定、一次消费；协议两代共享语义 |
| CredentialBroker/现有凭据边界 | 私有秘密引用和有界 worker；不向模型/Result 返回 secret |
| Session evidence / Coordinator / Arbiter | 唯一提示符与输入证据、人机仲裁、提交和完成事实 |
| History（规划） | 保真录制、已提交记录、容量/恢复/回收；不管理诊断任务 |
| Evidence / 导入资源（规划） | 来源无关的证据引用、独立资源授权与解析；通过来源端口读取原文 |
| AgentManager / Runtime（规划） | 应用级聊天与回合、Driver、事件、取消和上下文 |
| UI model / controller | 只读快照和本机意图；不持可变 AccessStore，不判定后端能力 |

目标是每项可变状态有一个明确 owner；观察者只保存 ID/弱引用，不复制 Job。当前Service已用unique_ptr拥有请求；CommandRuntime内部的共享引用仅用于同步回调和迟到发布保活，不跨组件暴露。ReadUseCases已可独立链接；执行已通过CommandSessionPort隔离具体Session；wait/replay由纯应用ObservationState拥有，平台验收差距按mcp/P0记录，P1 通用入口与 Session 适配已实现，P4/P6 产品集成和平台范围见 P1 实施记录。
QObject 统一选择 parent 或 unique_ptr 所有权；Session 观察者在适配边界使用 QPointer，worker 不保存 Session/UI QObject 裸指针。

## 4. 身份、资源、权限与可用性

ClientId、PeerId、RequestId、TaskId、SessionId、AttachmentId、Epoch、OperationId、CaptureId、DiagnosticId、ExecutionId 为不可互换值类型；输入/控制/提示符代际与观察 revision 分开。ChatSessionId、AgentTurnId、ToolCallId、外部 ThreadId 同样分开。

SessionBinding 绑定 session/attachment/epoch/连接 generation/可选目标指纹。连接代际不是环境代际：同一 UART 可 U-Boot→Linux，同一 shell 可进入 REPL、su、嵌套 SSH。环境 reset/接管/身份变化使探测缓存、Recipe 预留和旧就绪证据失效。

资源使用带判别绑定：
- 活动 Session：当前连接、环境、授权版本、目标身份。
- 持久 HistoryCapture：独立存储代际、范围与资源授权；不是短期观察 capture。
- DiagnosticResult：任务结果代际、来源快照、owner 和授权。
- ImportedFile：用户明确共享资源 ID、内容 hash、大小和授权；模型不得传任意本机路径取文件。

Principal 只能由已认证/可信本机入口创建，不能接受模型参数 clientId 冒充身份。内置 Agent 与外部 MCP 的启停和身份独立，底层政策共享；外部 MCP 关闭不能被偷换为内置 Agent 的隐式总开关。

权限分离：ReadOutput、固定 DiagnosticId、RiskControlledCommand、ScriptPublishAndInvoke、FullTerminalInput、ManagedElevation，以及后续 HistoryRead/EvidenceRead/DiagnosticExec/FilesystemWrite。
取消读取级联撤销执行；取消完整输入撤销提权；开启完整输入可同时选择读取；凭据变更撤销提权。行 GrantRevision 做 UI 乐观校验，ClientAccessVersion 保持 token 失效语义。旧行重连撤权必须拒绝并排队刷新，不能在 setData 栈内删树。

权限选择不等于执行可用：五类传输可保存已知选择，但 Profile、路由、提示符或可信身份缺失时明确 unavailable，零执行。基础 text/key/paste 不要求 Shell；command 快捷动作要求当前输入就绪；托管提权要求可信身份与已配对适配器，不根据 Password 提示猜测密码接收者。

## 5. 执行、确认、幂等与停止

分开请求交付状态和真实执行生命周期：
- 固定诊断：验证→预留→提交→观察→收口→终态。
- 风险命令：增加确认。
- 脚本：增加发布/调用；发布结果区分 NotPublished、Published、Unknown，保留“已发布未调用”事实。
- 正常/失败/取消/超时竞争只完成一次；迟到结果只作事实收口，不重复回复或泄露撤权后正文。
- 确认绑定 principal、目标/授权、策略/Profile、请求摘要、提示符/用户代际及单调过期；返回后重验再一次性消费。完整输入的既有授权不改为逐条风险确认。
- 接纳、出队、确认后、发布后、每次输入/Recipe 步骤和结果交付前都重验。
- operationId/票据精确重试不重复输入；变内容、过期序号/代际拒绝。Transport submitted 仅证明本地提交，不证明远端 delivered。
- TargetReservation 先持久标记后提交，预留在发布→调用间移交；未知结果析构保留 quarantine。只有证实未开始/已终止才显式释放；本机解除绑定目标+执行 ID，不暴露 MCP 解锁工具。

停止顺序 StopAdmission→CancelUnsubmitted→SettleInFlight→ReleaseOwnedResources。停止模型、工具等待和远端程序是不同事实；未知结果不自动重试、清标记或宣称成功。GUI 不无限 join/waitForDone，worker 使用受保护邮箱。

## 6. 通用能力与执行路由

Definition 保存稳定 capability ID、版本、实现状态、资源类型、输入/输出 Schema、策略和调用面映射。Registry/Schema 为 MCP、本机 Agent、未来 CLI 提供同一真源；未实现工具不注册，逐目标可用性通过列表返回，不随当前标签动态增删工具。

TargetEnvironment 单独表示 Unknown、POSIX/Linux、PowerShell、Cmd、Bootloader、DeviceCli，记录来源和 revision。工具发现不升级授权，不对 Unknown/U-Boot 自动发送 POSIX 探测。

Recipe 路由选择必须显式：
- 独立 exec/helper：不冒充当前 shell 的 cwd/env/临时身份。
- shared_terminal：复用 Coordinator/Controller + 唯一 Arbiter；先说明占用，要求就绪；遇 TUI/REPL/登录/文件传输拒绝，不自动 Ctrl+C/退出程序。
- 串口/Telnet/Custom 的已识别 Linux 可在诊断专属授权和就绪下运行有限 Recipe，不要求授予任意自由输入。
- 不因失败自动切换路由重试；脚本写入不可降级为正文逐字输入；不直接 Transport::write。

## 7. 四类数据不能互相替代

| 数据 | 语义与生命周期 |
| --- | --- |
| TerminalStateCache / Scrollback | 屏幕/有限摘要，可过滤/去重；不是保真日志 |
| 观察 capture/cursor | 约 60 秒的有界固定投影与增量观察，遵守旧/full/compact 契约 |
| Raw History | 用户开启后保存终端接收字节，保留重复、ANSI、CR/LF、NULL、非 UTF-8；缺口显式 |
| Agent output artifact | 跨回合工具结果预览/原文引用，有 owner、配额、TTL，不是永久 History |

History 在文件传输分流之后、Framer 之前只观察一次，背压重投不重复录制；文件传输载荷用边界事件说明省略。录制默认关闭，共享历史另授权；不录本地输入、密码缓冲或认证私有通道，远端敏感回显不保证自动脱敏。认证隔离/privacy gap 在适配器验收前不能声称已有。

EvidenceRef 可来自 terminal_capture、diagnostic_result、imported_file，记录 hash、原始区间、来源/版本/完成/截断、授权和期限。证据引用不是授权令牌；源被清理返回 EVIDENCE_EXPIRED；结果无持久化授权时只存有期限内存证据，不偷偷开启录制。AI finding 是带证据与限制的推断，不是已证实根因。

## 8. Agent 与 UI 的目标契约

内置模型 Driver 负责模型→工具→观察→模型循环；外部 Driver 托管另一套运行时。首版 Qt 原生一种确定协议，其他协议与 Node sidecar 仅按实际需要独立评估，不把 JS SDK 放入 Core。
统一事件包括 turn_start/end、model/reasoning_delta、tool_call/result、approval_requested/resolved、plan/file/search、compaction、usage/performance、error；事件绑定 chat/turn/tool/execution ID 和顺序号。正文批量更新，批准/终态有序交付。

同聊天一活动回合；不同聊天写同终端继续服从 Arbiter。ChatSessionStore 保存消息、活动、usage、压缩边界和后端身份；UI 隐藏不销毁 Runtime，重启恢复不重发历史工具。
ContextManager 保证 tool call/result 配对、协议 continuation、动态 token 预算、预回合/step/413/手动压缩、目标/权限/任务重新注入。事实记忆不是授权真源。OutputArtifactStore 读取原结果，不通过重执行恢复输出。

聊天模式是底层授权之上的收紧层：Observer 不执行；Confirm 对其暴露的执行动作有真实批准边界；Auto 不自动授予权限。既有 FullTerminalInput 本来免逐条确认，Confirm 不得静默暴露未加批准的任意 text/key/paste。
NovaTerm 当前终端命令会保留 cd 等环境；不能照搬 Netcatty 隔离子 Shell 的提示。NovaTerm run_script 不等于 Netcatty JavaScript nct.* 自动化脚本库。

Ela 侧栏包含 Agent/模型/推理/权限、消息与工具/批准卡片、选区/附件、历史/停止；后续增加 Skills、导出、分支。撤销聊天不回滚远端副作用。
外部 resume 身份绑定 backend/bin/runtime/authMode，不跨 SDK/App Server 续接。steer、图片、模型目录、原生提问等按 DriverCapabilities 显示。全局 Agent、CLI 和自动开主机均为独立扩展，不从工具规格推断已实现。

## 9. 预算、线程与安全

当前兼容预算不得与未来扩展预算混用：

| 当前范围 | 保留值/语义 |
| --- | --- |
| 接入/调度 | MaxClients=4；每 peer 2 活动/4 排队 |
| 等待 | 每客户端/会话一个；全局 32；最多 30 秒 |
| 基础采样 | 每 Session 250 ms 合并，无等待者无常驻采样；显式 StrictRevision/BoundedStaleness |
| capture | 每连接 4；客户端 scope 每客户端 4/全局 32；TTL 60 秒，分别计额 |
| 输入/控制 | 64 动作，原文 16 KiB、编码 64 KiB；32 控制会话；空闲 60 秒 |
| 投影 | compact 默认 4 KiB/24 行，最终双 payload 编码 ≤8 KiB；full 默认 64 KiB/256 行，硬限 256 KiB/1024 行 |
| full/compact | full 截断 resetRequired=true/nextToken=null；compact 区分 projectionLimited 与 sourceGap，补读不续租 |
| 发布/记录 | 脚本 2 MiB、待发布 8；每客户端执行记录 32、执行+发布总 128 |
| 命令/确认 | 默认输出 64 KiB；确认 60 秒、发布 120 秒；HTTP 原报文/帧/SSE 额度不变 |

诊断任务全局 32/Principal 4 已在 P1 TaskManager 实现并做行为测试；其余未来初值：历史单 Session 队列 16 MiB/全局 64 MiB、chunk 4 MiB；扫描并发全局 2/Principal 1、页扫描 2 秒/16 MiB；历史单结果默认 ≤64 KiB。这些是设计值，不是性能成绩。

Session/授权/应用状态串行于所属 GUI 线程，网络、Parser、文件/SFTP、搜索和凭据各走异步边界。worker 仅交换有界值或不可变引用；拒绝符号链接重定向，私有文件含 WAL/SHM 与派生索引统一清理，不承诺 SSD 物理擦除。
秘密不进入工具结果、普通输入批次、capture、日志/审计。审计记录能力、匿名目标、状态/耗时，不记录 Bearer/密码/脚本正文。远端输出、附件、标题均为不可信数据。

## 10. 文档关系

对外精确契约统一在[02当前接口契约](02_Current_MCP_Contract.md)，原项目P8的历史证据和被替代设计分别位于[03实施与验收](03_Implementation_and_Verification.md)、[04历史设计档案](04_Historical_Design_Archive.md)。原文件无需继续承载规范或追溯。

执行顺序和状态见 [阶段划分](01_Phases_and_Acceptance.md)，每阶段任务只扩展其范围。
当前接口说明见 [MCP 使用说明](../../../MCP_Usage.md)，历史验证见 [实施与验收记录](03_Implementation_and_Verification.md)。

## 11. 设计补充契约（2026-10-18）

本节补齐跨阶段语义，不修改 02 现行九工具契约。P1 的环境事实与类型化准入已按 2026-10-19 实施记录落地；模型发送、历史与证据部分仍是后续规划。具体接口和失败用例分别归所属阶段。

- **模型发送边界**：本机读取、向指定模型服务发送、持久保存分别授权。ContextManager保留消息/摘要/artifact的来源依赖，ModelEgressPolicy在每次模型请求提交前校验接收方和来源；切换服务、撤权、摘要恢复不绕过校验。已发送数据无法追回，不声称撤权可删除远端副本。详见[P6补充契约](P6_Agent_Runtime_and_UI.md#补充契约模型发送与派生内容授权)。
- **环境事实边界**：Session适配层唯一拥有EnvironmentTracker，区分环境声明、可信就绪和目标身份。普通输出只作候选，不能建立执行就绪；证据失效或切换不确定时退回Unknown并提升revision。详见[P1环境契约](P1_Capability_Runtime.md#补充契约环境事实与失效)。
- **录制完整性边界**：已提交前缀与正常封口分开。无有效封口即unknown_tail，缺失数量可未知；不得用最后一条完整记录推断采集完整。详见[P2持久性契约](P2_History_Recording.md#补充契约持久提交与异常尾部)。
- **搜索连续性边界**：首版采用有界服务端SearchSession，cursor绑定查询和投影版本；跨页保存解码/匹配状态，到期明确拒绝，不从任意字节重新解释。详见[P3续扫契约](P3_History_Search_and_Evidence.md#补充契约搜索续页与有界状态)。
- **能力类型边界**：统一的是元数据、准入/取消和授权生命周期；领域请求/结果通过类型化端口传递，不建立中心全系统variant。异构协议解码在对应能力适配器完成。详见[P1分派契约](P1_Capability_Runtime.md#补充契约领域类型化分派)。

所有预算新增值均为待验证的设计初值。首个只读交付与执行交付分别按[阶段划分§6.1](01_Phases_and_Acceptance.md#61-增量交付闸门)验收，局部交付不将P0/P1整阶段标为完成。
