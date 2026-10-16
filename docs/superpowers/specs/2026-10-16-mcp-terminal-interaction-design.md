# MCP 终端操作闭环设计

日期：2026-10-16。状态：重设计稿，待审阅，尚未实施。
本稿完整替代同文件先前草案：取消要求 Agent 手动组合 acquire/send/release 的方案，
采用“观察—操作—再观察”的统一交互协议。当前程序行为仍以源码和 P8 实施记录为准。

## 1. 目标和已确认需求

Agent 在用户看得见的同一终端中工作，像人一样查看屏幕、输入命令、按 Enter、
观察输出、回答交互提示，并根据反馈决定下一步。执行过程保留当前连接、目录、
环境变量、解释器和正在运行的程序，不另开自由命令执行通道。

已确认的产品要求：

- MCP 接入界面按客户端、会话勾选“允许执行命令”；勾选后无需逐条命令确认。
- 同时支持一次命令的结果跟踪，以及可跨多轮调用的自由终端交互。
- 滚屏期间可以读取新观察；命令运行、命令结束和下一条可输入分别表达。
- 会话可分别配置 sudo、su 认证密码；Agent 请求使用，密码由 NovaTerm 托管。
- 用户键盘和粘贴优先，随时接管；撤权阻止尚未提交的 Agent 输入。
- 在同一交互闭环上减少模型调用和重复输出：普通滚屏在本地合并，默认事件等待、
  小预算增量返回，详细输出按需补读；不以省 token 为由隐瞒不确定状态或输出缺口。

“模拟人”指同一终端、同样的输入编码和可观察反馈，不人为逐字符延迟。
“正确执行”分成：输入对象正确、输入内容正确、操作不重复、开始/结束证据明确、
结果符合预期。接口能验证的证据必须公开；不能把退出码 0 等同于业务目标完成。

## 2. 为什么选择混合方案

| 方案 | 优点 | 局限 |
| --- | --- | --- |
| 只模拟文本/按键 | 通用，接近人操作 | 难以知道命令结束、回显是否等于执行、下一步输入是否安全 |
| 只提供 exec/run_command | 命令结果清楚，便于自动化 | REPL、长任务、确认提示和设备 CLI 不适合一次调用阻塞到底 |
| **终端闭环 + 可选 Shell 集成（采用）** | 普通程序用屏幕交互；有集成时增加可靠状态，长短任务统一 | 必须明确区分已观测、已确认和未知，仍不能替 Agent 判断所有业务结果 |

Agent 日常只需“观察终端”和“操作终端”两个工具。服务端管理输入所有权、身份、
顺序和异步状态；Agent 不必直接操纵 Lease 或为长任务维护并行工具调用。

## 3. 基于现有实现的改动边界

当前源码具备：

- MCP Streamable HTTP、客户端认证和运行会话目录。
- SessionInputArbiter 的唯一 MCP Lease、用户抢占、文件传输互斥。
- TerminalContextProvider 的有界屏幕、近期输出、revision 和不可变快照。
- InteractiveStreamFramer/SessionCommandCoordinator 的提示符和完成事件。
- CredentialStore 的系统凭据库与内存回退。

当前缺口：

- run_command 等待最终结果，缺少跨调用追加输入和统一的活动命令观察。
- read_context 的 state 是 Session 状态；没有命令状态或明确的下一步输入就绪信息。
- 输出变化靠主动读取，缺少异步等待变化；滚屏不能与执行进度关联。
- Core 编码输出目前可能被当作用户输入，必须保留 Agent/User/Protocol 来源。
- 当前密码存储不能直接等同于安全的交互式 sudo/su 认证适配器。

不重构核心去 Qt 化、不修改 ITransport 虚函数、不新增第二个 Transport 输入泵。
新核心请求数据用标准库类型，Qt 桥接留在现有门面和 Session 层。

## 4. 对 Agent 的工具

保留 novaterm_list_sessions、novaterm_search_context。
扩展 novaterm_read_context，新增 novaterm_interact_terminal、
novaterm_request_elevation。其余旧命令/脚本工具保留兼容，详见 §12。

所有新版操作和观察返回同一种 observation；参数严格校验，结果用现有 MCP 封装。
工具目录说明明确要求：先观察，使用返回的输入令牌操作，依据反馈继续。
不会发送一条通知就假定模型一定被唤醒；标准请求/响应闭环是主要使用方式。

### 4.1 list_sessions：发现会话与能力

每条会话返回：

- sessionId、epoch、名称、连接类型和 sessionState。
- permissions：readOutput、terminalInput、useElevation。
- availability：inputAvailable、disabledReason、controlState、ownedByThisClient。
- shellIntegration：available；elevationMethods：当前可用的 sudo/su。
- capabilities：保留旧字段，新增 terminal_interaction、managed_elevation。

授权和临时可用性分开：允许输入但正在文件传输时，terminalInput=true、
inputAvailable=false、disabledReason=FILE_TRANSFER_BUSY。
没有执行权限时仍可读取已共享会话，但不返回可用输入令牌。

勾选更新会话权限修订号，后续 list/read/interact 读取最新值；执行前也重新验证。
首版不新增依赖特定客户端的权限推送机制，客户端按工具说明重新发现能力。

### 4.2 read_context：看屏幕或等待变化

必填 sessionId、epoch；可选 sinceToken、waitMs、maxBytes、maxLines、includeViewport，
以及 responseMode=compact|full、waitFor=attention|completion|change。
waitMs 为 0–30000；旧调用不指定新参数时保持 waitMs=0 和既有返回行为。
新版 Agent 工具说明引导使用 compact、waitFor=attention、waitMs=30000。
有 sinceToken 时按 waitFor 异步等待，没有 sinceToken 时立即返回当前快照。
默认 attention 不因普通输出变化或 command 从 submitted 变成 running 就立即返回，
具体返回条件、交互延迟与补读规则见 §8.1。

返回：

- observationId、capturedAt、modelRevision、eventRevision、sessionState。
- viewport、cursor、alternateScreen、recentOutput。
- outputChanged、screenChanged、sourceTruncated、outputTruncated、resetRequired。
- command、inputReadiness、control、elevation；本人控制会话时另含 lastOperation。
- nextToken：下一次增量读取的位置；inputToken：下一次输入的前置条件。
- wakeReason：initial / command_finished / attention_candidate / diagnostic_candidate /
  control_changed / output_changed / state_changed / quiet / deadline。

modelRevision 表示终端模型变化，eventRevision 表示会话控制/命令/权限等状态变化；
读取游标同时携带两者，所以“命令结束但没有新文字”也会唤醒等待器。
普通 revision 变化只更新本地状态；仅达到 waitFor 的返回条件时结束工具调用。
screenChanged 不能被解释为一定滚屏；outputChanged 不能被解释为当前命令仍在执行。

高输出时合并观察：内部基础快照发布间隔不短于 250 ms，不能据此每秒给模型返回
四次结果。compact 默认文本预算 4 KiB/24 行，并以最终编码工具结果 8 KiB 为上限；
元数据和兼容字段也计入。full/旧行为保留 64 KiB/256 行默认值和既有硬上限
MaxTextBytes=256 KiB、1024 行。默认精简投影和可恢复补读见 §8.1。
不新增无界日志或订阅者输出队列。

等待期间不阻塞 GUI 线程；命令完成、抢占、断连、权限或隐私状态变化优先唤醒。
无变化到期也返回观察，wakeReason=deadline，不认为命令已结束。
quiet 只作交互反馈合并，表示 150 ms 内没有观测到新输出，不是成功或空闲证明；
默认 attention/completion 不因构建输出中偶发静默提前返回。
full/旧行为保留截断后的 resetRequired。compact 区分主动投影、省略和源数据缺失，
不能把每次预算截断变成强制重发整屏；摘要不保证保存每行或区分 stdout/stderr。

### 4.3 interact_terminal：做一次操作，并返回观察

输入：sessionId、epoch、inputToken、operationId（客户端 UUID），actions、
waitMs（0–30000）、waitFor、responseMode（默认 compact）。每个动作单独用 type 判别。
command 默认 waitFor=attention、waitMs=30000，先在本地等待有用结果；
text/key/paste/line_ending 默认 waitFor=change、waitMs=1000，保证交互响应；
release 立即返回。调用方可显式缩短等待，不因长等待延迟用户接管。

动作：

| type | 含义 |
| --- | --- |
| text | 输入普通 UTF-8 文本，不自动回车，不允许 C0/DEL |
| key | Enter、方向键、Escape、功能键，或单字符加 Ctrl/Alt/Shift；如 Ctrl+C |
| paste | 沿用当前括号粘贴和换行编码，可包含多行文本 |
| line_ending | 按设备配置发送 CR/LF/CRLF |
| command | 单行命令 + 一次提交按键，并创建命令跟踪记录 |
| release | 交还终端输入权，必须作为本次唯一动作 |

单次最多 64 动作，原文合计 16 KiB，编码后最多 64 KiB；每会话一个待写批次。
command 不能混入其他动作；含控制字符/换行的 command 被拒绝，多行交互用 paste。
批次按顺序编码提交；用户抢占可以打断批次，不能声称用户输入与 Agent 输入绝对原子。

command 是输入便利操作，执行的仍是可见终端文本和 Enter/设备换行，不是独立 exec。
它要求当前命令行就绪；支持 verified（集成协议证据）和 observed（显式设备提示符）
两级，结果必须标明证据。Agent 可传 requireVerifiedReady=true 要求更严格的条件。
没有提示符证据时 command 返回 CONTEXT_NOT_READY，零输入；text/key/paste 仍可用，
Agent 可基于屏幕明确操作。提示符因此增强命令操作，不成为所有交互的硬依赖。
已有命令行残留输入或活动命令时不得直接拼接新 command；不自动 Ctrl+U 或清空用户输入。

动作接纳后，按上述策略本地观察，达到条件或等待到期后返回：

- operation：id、status、inputMayHaveBeenSent、acceptedActionCount。
- observation：与 read_context 同形的当前观察。

operation.status 为 queued / submitted / interrupted / failed / unknown。
submitted 只表示编码字节已交给本机 Transport 写入口，不代表远端收到或命令结束。
现有 ITransport::write 没有发送确认，不返回虚假的已送达字节数。
Parser/Transport 尚未接纳时也可先返回 queued，之后观察能读取最后操作进度。

返回等待超过 waitMs 后结束的是本次工具调用，远端程序继续运行。
命令和控制状态保存在 Session 中，后续 read_context 或 interact 可继续。
客户端即使只支持串行工具调用，也能观察构建、安装和交互程序。

release 不发送 Ctrl+C、exit 或断连；中断程序须显式 key: Ctrl+C 并观察反馈。
释放输入权不意味着远端程序退出或 root 权限被撤销。

## 5. 命令进度与下一步输入：分别表达

command 返回的结构：

- executionId：本地关联记录，不能当作已验证的远端进程身份。
- state：none / submitted / running / finished / unknown。
- result：success / failure / unknown。
- exitCode：整数或 null。
- evidence：shell_integration / device_prompt / screen_inference / none。
- terminationConfirmed：有无已观测的命令结束边界。
- outputAttribution：shared_terminal；可能包含后台日志。
- promptGeneration：可用时提供；当前等待提示不能由 # 或 $ 单独证明。

只有关联的集成开始事件才能从 submitted 提升到确认 running。
只按屏幕猜测的状态要标 screen_inference，不能标为确认事实。
集成完成事件和明确退出码可确认结束及程序结果；设备提示符返回可按其 Profile
识别结束边界，exitCode/result 仍可未知。后台进程可能在前台命令结束后继续输出。

inputReadiness 分成：

- canInteract：本人现在能否继续向当前程序输入，不等于可以发新 Shell 命令。
- canSubmitCommand：是否可以在已识别的命令行开始一条新命令。
- confidence：verified / observed / unknown。
- context：shell / application / alternate_screen / unknown。
- reason：权限、占用、未就绪、认证中等具体原因。

例如构建运行时可 canInteract=true（可 Ctrl+C）、canSubmitCommand=false。
构建结束后返回已就绪提示符，可 canSubmitCommand=true，即使后台日志仍在滚动。
密码提示时只读状态可显示 authenticating，普通 Agent 输入由认证隔离规则决定。
提示符正则不是可信进程身份验证，也不能保证当前行为空；只有正则时标 observed。

不增加无依据的“等待 y/n”事实字段。Agent 从屏幕判断交互提示；
服务端只在程序集成或认证适配器有明确证据时报告对应状态。
长期静默、读取超时或一次工具返回都不能推出命令成功或下一条就绪。

自由 text/key 操作若触发了可关联的 Shell 集成开始事件，也建立跟踪记录，不要求
Agent 一定使用 command 快捷动作。没有集成时，text/key/paste 仍然可用，命令边界
保持未知；不根据输入中的换行自行伪造 Shell 命令记录。

命令记录最多每会话一个当前记录和一个最近终态；终态最多保留 10 分钟。
不保存命令全文或逐命令完整输出；保留正文摘要、执行证据及有界元数据。

## 6. 像人操作的典型闭环

### 6.1 普通命令

1. list_sessions 发现具备 terminalInput 的目标。
2. read_context 读取屏幕和输入令牌。
3. interact(command: pwd) 默认等待有用事件，短命令直接返回终态与必要输出。
4. 若未结束，read_context(sinceToken, waitFor=attention, waitMs=30000,
   responseMode=compact) 在本地等待，省去“已接纳/开始运行”的模型往返。
5. 观察到命令结束且 canSubmitCommand=true，再提交下一条。

### 6.2 正在滚屏的构建

1. interact(command: make) 默认本地等待最多 30 秒；在此期间结束则直接返回终态。
2. 出现可信完成、需要关注的线索或用户接管时提前返回；普通编译滚屏只在本地合并。
3. 等待到期仍运行时返回精简状态和最多预算内的新增尾部；Agent 可继续 attention
   等待。若明确需要观察普通进度，再显式使用 waitFor=change，不默认高频采样模型。
4. make 退出但后台仍打印日志时，命令状态独立更新，不等待整个终端静默。
5. 只有输出而无可信完成证据时保持 unknown/未确认，不能自行假造退出码。

两次观察的关键字段示意（省略身份、屏幕和其他字段）：

    执行中：outputChanged=true，command.state=running，
            command.terminationConfirmed=false，canInteract=true，canSubmitCommand=false。
    已结束：command.state=finished，command.terminationConfirmed=true，
            command.exitCode=0，canSubmitCommand=true；outputChanged 仍可能为 true。

没有集成证据时第一种观察应如实使用 submitted/unknown，不用滚屏推断 running。

### 6.3 程序询问是否继续

1. Agent 从屏幕看到确认提示；执行记录仍为活动或未知。
2. interact(text: y, key: Enter) 返回新的观察。
3. 活动命令存在时，Agent 必须附带 replyToExecutionId；发送前核对它仍为当前记录，
   防止程序已退出后把迟到应答写进下一条 Shell 命令。
4. 没有可跟踪命令的自由交互仍凭 inputToken 验证本地上下文，结果如实保持未知。

### 6.4 REPL、编辑器与取消

text/key/paste 可持续操作 Python、设备 CLI 和 TUI；当前屏幕、光标与备用屏状态
是主要上下文。不模拟点击终端 UI，也不要求每一步回到 Shell。
Ctrl+C 只是发出按键；收到完成证据前不标记程序已终止。

## 7. 输入所有权、前置条件和去重

MCP 接入界面授予的是能力；服务端在首次有效操作时自动取得 Arbiter Lease。
控制权绑定认证客户端、Session、attachment、epoch 和 controlGeneration，
不绑定单条 HTTP 连接、SSE 响应或某一代 MCP 传输会话。

inputToken 是不可伪造的短期输入前置令牌，绑定：

- 客户端、实例、Session、attachment、epoch。
- 权限代际、用户输入代际、控制代际、上下文隐私代际。
- 当前控制身份或空闲状态，以及下一输入序号。
- 捕获时已知的 promptGeneration、活动 executionId 和 30 秒有效期。

不要求每次输入都匹配完全相同的 modelRevision：持续滚屏会改变它，不能导致
Agent 永远无法输入。但用户键盘、权限变化、命令结束/提示符变化等相关上下文
变化会使旧输入前提失效。下一次输入需要重新观察，而非盲目重放。

同一会话一次一个 Agent 控制者；其他 Agent 可按授权观察，写入返回 TERMINAL_BUSY。
全局最多 32 个有效控制会话。无活动 60 秒释放；控制者有效观察或输入可续期，
一次观察等待期间保持租约，完成后续期；失联不自动终止远端程序。

operationId、输入序号和规范化动作摘要一起去重：

- 相同已接纳操作只返回接纳记录和当前有权查看的观察，不重复写入。
- 同序号不同内容返回 INPUT_SEQUENCE_CONFLICT。
- 小于高水位返回 INPUT_ALREADY_SUBMITTED；大于期望值返回 INPUT_OUT_OF_ORDER。
- 当前批次接纳后即推进高水位，随后部分失败也不允许整批重发。
- 失效令牌、旧控制代际或权限变化始终拒绝，不因缓存过期重新写入。

每控制槽只保留高水位及最近操作摘要/接纳结果，不无限保存 UUID 或正文。
合法的精确重试先校验当前认证/授权及已接纳记录，才能返回公开接纳状态；
记录之外的请求必须通过全部令牌检查。回复丢失不等于操作未执行。

## 8. 用户优先、输出刷新和预算

用户操作通过现有 Arbiter 立即抢占；停止未提交 Agent 字节，释放控制，
更新代际并唤醒等待器，再放行用户输入。已交给 Transport 的字节不能撤回。
撤权、关闭、重连、换绑和删除客户端同样使输入代际失效。

Core 编码请求和结果保留 User/Agent/Protocol 来源、控制代际、操作 ID、
Session generation。DSR/DA 等协议回复不算用户接管；Agent 输入不抢占自身。
所有 Parser 回调及每次写入前再次核对令牌、权限和 Session 生命周期。
用户输入不能因一个长等待请求而被排队阻塞。

复用输入泵和 Parser 发布物，按 revision 事件异步唤醒等待器，合并高频通知。
没有等待者时不增加持续屏幕采样定时器；等待者存在时最多 4 Hz 请求采样。
最终快照只能出自 Parser/Provider，不让 MCP 或 UI 修改终端模型。
这是 NovaTerm 内部观察频率，不是向 Agent 返回结果或调用模型的频率。

每客户端/会话最多一个等待器，全局最多 32；仍受现有每连接 2 活动/4 排队预算。
同一客户端发新 interact 时先唤醒该会话旧 read 等待器，避免等待占槽妨碍应答。
一个输入批次最多 64 KiB 编码缓冲，全局最多 32 批/2 MiB；队列必须可停止、统计。
文件传输、旧命令和新版交互共用唯一 Lease，相互占用时返回明确原因。

RPC 取消只取消当前未提交操作或等待；已提交输入不得重放。
控制失效后命令可能仍运行，保留已证实终态，其他结果变为 unknown。
旧路径产生的 quarantine 仍阻止新输入，不通过新版接口绕过。

### 8.1 减少模型 token 的默认策略

本节只优化上述闭环的反馈时机和投影，不改变输入权限、命令证据及提权保密边界。
NovaTerm 用本地有界规则完成筛选，不为“总结日志”额外调用一个模型。

**返回时机。** 三种 waitFor 的契约：

| waitFor | 提前返回条件 | 适用场景 |
| --- | --- | --- |
| attention（命令默认） | 可信命令完成、提示符返回、认证状态变化、控制/权限/断连事件、一次新的交互或诊断线索 | 构建、安装及一般命令 |
| completion | 可信命令完成/提示符返回，或控制/认证等必须处理的事件；普通诊断不打断 | 明确不需要逐步观察的批任务 |
| change（按键默认） | 合并后的屏幕/输出或有意义状态变化 | REPL、编辑器、显式查看进度 |

所有等待最多 30 秒；用户抢占、撤权、断连和认证隐私变化不受节流延迟。
command 的开始事件通常只更新内部记录，不单独唤醒模型；最终结果包含该记录。
对于已确认仍在运行的非交互命令，change 模式普通进度合并到最多每秒一次返回；
这不是额外服务端定时调用，仍须客户端发请求。显式 waitMs=0 保留即时检查能力。

交互线索可以来自显式程序/设备事件，或有限的末行模式（如 y/n、确认选项、输入
提示）与 150 ms 静默共同判定。通用文本检测只是 attention_candidate，不得报告
为“确定正在等输入”或自动应答；可能漏检，因此必须保留 30 秒观察兜底。
诊断检测只匹配有界、明确配置的模式，返回 diagnostic_candidate；不能因日志每次
出现 error/warning 就唤醒模型。按 executionId、匹配类型及规范化末行指纹去重；
同一未解决线索直到新输入、执行上下文或匹配内容变化前只提前返回一次。
多次命中不积累响应，只保留固定数量（最多 4 条）的线索摘要。

**返回内容。** compact 默认：

- 最多 4 KiB/24 行文本；整个最终编码工具结果最多 8 KiB，优先保留身份、操作接纳
  情况、命令/就绪状态、输入令牌、缺口标记，再分配文本空间。按 UTF-8 边界裁剪。
- 正常读取只返回自上次游标以来的新增文本；未变化时不重复 recentOutput。
- 首次/恢复观察返回当前光标附近及底部的必要行，附绝对屏幕行号、总行列数及
  viewportPartial。其他 compact 读取默认省略未变化的 viewport；REPL/TUI 或
  screenChanged 时按预算提供相关当前行。若与 recentOutput 重复，优先屏幕表达，
  不把同一行同时放进两份文本。full/includeViewport 显式请求可取完整有界屏幕。
- 完成时优先返回退出码、下一条就绪信息、受限诊断上下文和新增输出尾部；如果
  内容已经全部交付，不重复全文。短命令的实际输出照常返回，不能一概只保留状态。
- 不在返回中回显输入命令、输入动作、工具说明和冗长的自然语言“总结”。元数据
  用简洁的固定 schema；保留判断输入/完成安全所需的所有字段。
- 进度条反复重绘保留最新值；确实连续相同的文本可以本地压缩为 repeatCount。
  不合并不同命令、不同内容的错误或有业务意义的不同数值，不排序原始输出。

maxBytes/maxLines 可显式上调至既有硬上限；full 用于诊断、复杂 TUI 或补充上下文。
不宣称字节数等于 token 数；实际 token 取决于文本、分词器和客户端交给模型的内容。

**预算与补读。** 新增 projectionLimited（因预算/筛选未展示全部）、
omittedLines、sourceGap（底层摘要已淘汰，不能补齐）及可空 expandToken。
纯预算裁剪不设置 resetRequired，也不要求客户端无限重新获取整屏。
nextToken 是已观察源位置，允许正常进度继续向前；expandToken 则固定引用本次
已保留 capture 内尚未展示的区域。Agent 需要详情时用 read_context(expandToken,
maxBytes/maxLines) 分页补读，不在补读时重新执行命令或返回另一时刻的混合快照。
expandToken 与 sinceToken 不能同时传入；补读不进行事件等待，只使用已保留快照，
必须验证认证、权限、epoch 和隐私代际，且不得更新正常观察的游标或输入所有权。

补读复用既有有界 capture 槽位和 60 秒过期策略，不另外保存无界命令日志。
不存在/被淘汰的 capture 返回 CAPTURE_EXPIRED；源摘要本来已有缺口时 sourceGap=true，
不能声称完整日志可恢复。行级缺口无法可靠计算时 omittedLines=null，不能伪造计数。
错误/交互线索尽可能优先展示，但无法保证本地规则识别所有关键行；Agent 根据
projectionLimited/sourceGap 决定是否补读、扩大预算或请求用户可提供的其他记录。

**协议与上下文开销。** 当前 toolResult 把 payload 同时放入 structuredContent 与
text content，客户端可能将两份都交给模型。首版保留兼容契约，8 KiB 限额对最终
两份编码后的完整结果计数；不能只测 structuredContent 或声称已经消除双份开销。
先裁小公共 payload，不额外附加一份重复解释。未经客户端互操作验证，不擅自
去掉 text 或 structuredContent，避免 Agent 看不到屏幕或输入令牌。
工具说明和 schema 保持稳定、简短，不在 list_sessions 中重复全部工具说明。

**等待期限。** 新版业务调度与 WireSession 必须按 waitMs 设置期限：本地等待预算
之外预留 5 秒传输/编码余量，不得继续使用当前 read 的 2 秒固定期限。
若客户端不支持所需等待时长，允许显式缩短 waitMs；未收到响应不代表命令未执行，
客户端仍按 operationId 去重。短期限降级增加往返，不能因此关闭安全状态或重复输入。
输入令牌在结果真正返回时依据最新有效上下文签发，不复用等待开始时即将过期的令牌。

**成本验收。** 不预报未经实测的 token 节省百分比。确定性夹具统计：响应次数、
最终编码结果字节、实际文本行数、重复行比例、补读/源缺口次数及关键事件反馈延迟。
这些指标不是账单 token；如需真实 token 对比，再用同一客户端、模型和上下文单独测量。
30 秒等待内、无候选事件且有可信终态的命令，无论输出多少次更新，都只返回一次
最终交互结果；无可靠终态的长任务到期最多返回一次精简反馈，不因普通滚屏连发。
候选交互从适配器/末行检测事件形成起应在 500 ms 内交付给已存在的等待请求，
不包括网络和客户端处理时间；命令完成与用户抢占不得因减少 token 等到 30 秒超时。

## 9. MCP 接入界面

会话主表：

| 会话 | 连接 | 读取输出 | 允许执行命令 | 提权凭据 | 允许托管提权 | 当前操作 |
| --- | --- | --- | --- | --- | --- | --- |
| 会话名 | 已连接 | 复选框 | 复选框 | 未配置/sudo/su/两者 | 复选框 | 空闲/Agent 操作/用户接管/认证中 |

- 执行默认关闭；勾选执行同时启用读取，取消读取同时撤销执行和托管提权。
- 勾选即授权日常输入与命令操作，不再弹出逐条命令确认。
- 权限按客户端/会话及连接 epoch 隔离，重启、重连或换目标后须重新授权。
- 当前操作显示控制者标签和有证据的命令状态，提供本机“接管”动作。
- 提权凭据列打开 sudo/su 分别设置的掩码编辑区；保存后不回填密码。
- 支持替换/删除，显示系统凭据库或“仅本次运行”；密码不成为表格数据。
- 配置密码不等于授权使用，托管提权须独立勾选，且依赖允许执行命令。
- 固定诊断、旧交互命令和脚本权限保留在“旧命令接口”详细区。

## 10. NovaTerm 托管提权

### 10.1 接口

novaterm_request_elevation 输入与 interact 共用的 sessionId、epoch、inputToken、
operationId，另含 method=sudo|su、mode=command|shell。
command 模式含 command，shell 模式不含。目标账号固定为本机配置的 root。
返回 operation/elevationId 和同形 observation；之后通过 read_context 跟踪。

Agent 只能请求“使用 sudo/su 执行此操作”，不能读取或任意指定填入秘密的位置。
不接受 password、credentialRef、可执行文件路径、提示符正则或 Agent 提供的认证脚本。
显式 shell 模式才进入持续 root Shell，不能将单次提权静默扩展成 root Shell。
su 登录环境可能改变目录，返回实际观察，不自动补 cd 或假定工作目录未变化。

effectiveUser、identityVerified、authenticationState 和 command 状态分别返回。
认证成功或提示符变成 # 均不能单独证明已成为 root；缺证据时返回 unknown。
Agent 手动输入 sudo/su 不触发自动代填，必须由该工具启动受控认证事务。

### 10.2 密码存储与使用

复用 CredentialStore：Windows Credential Manager，Linux/BSD Secret Service；
不可用及当前 macOS 后端仅内存。配置只存引用，密码不进入 Profile、SessionStore、
MCP 参数、返回值、日志、命令行、环境变量、临时脚本或滚动历史。

sudo 通常验证调用者，但 rootpw/targetpw/runaspw 可改变策略；界面分别标注
“sudo 认证密码”和“su root 密码”，由用户按目标配置，不自动尝试 SSH 登录密码。
分别存储凭据；Agent 不可通过 MCP 设置、导出、查看其长度或摘要。
凭据绑定本机运行用户及目标身份；SSH 包含主机密钥、端点与登录账号。

Session 级 ElevationBroker 绑定控制权、输入/权限代际和认证事务。
仅在明确支持的可信认证适配器可用时取密，发送前再次核对身份和权限。
认证最多提交一次，60 秒期限，失败/多因素/超时/接管即终结，不循环试密码。
密钥环访问不能阻塞 GUI；现有 Secret Service 的同步接口需增加非阻塞门面或在
正确拥有独立 DBus 连接的工作上下文中实现，不能直接跨线程调用原 GUI 连接。

### 10.3 认证通道和输出隐私

密码优先经受保护的本地 IPC 或独立 SSH 认证辅助通道交给受信任认证组件；
普通命令和输出继续经过原交互终端。辅助通道只做认证，不执行自由命令。
不能用 echo 密码管道、进程参数或普通 text/key 动作提交秘密。

现有普通 SSH PTY 仅有字节流，不能证明当前哪个进程在请求密码和是否已关闭回显。
只匹配 Password 文本不能满足本功能；本地和 SSH 的 sudo/su 适配器必须分别验收。
SSH 若需要目标端辅助组件，作为显式安装/配对依赖，不自动上传安装。
缺少适配器返回 ELEVATION_ADAPTER_UNAVAILABLE，不静默降级成向终端盲输秘密。
首版不在缺乏可信目标身份的 Serial/Telnet 上开放托管提权，普通交互继续可用。

取密前进入认证隐私状态：所有 MCP 客户端暂停该会话的屏幕/搜索/执行输出，
失效旧 capture/快照缓存并取消待发布结果；只返回公开认证进度。
认证期间阻止 Agent 改变接收者；用户仍能接管并终止代填。

认证秘密不进入 Parser、渲染器或滚动历史。异常回显在唯一 InputPump 的 Core
入口前隔离；已发现风险时维持读取隔离，不能仅在 JSON 返回时事后替换。
普通字符串脱敏仅作纵深防护，不能保证识别恶意的任意编码或拆分。
确认认证通道关闭且无残留秘密输出后推进隐私代际、恢复观察；不确定时要求本机处理。
取密缓冲尽力清零，不宣称 Qt/系统服务/远端组件的所有副本均可可靠擦除。

### 10.4 权限边界

保证正常接口不向 Agent 返回密码，信任 NovaTerm 本机与目标认证组件。
若 Agent 可执行任意 root 代码，它可能读取内存、修改认证组件、截获后续输入；
该方案不能宣称抵御恶意 root Agent。本机 root 与本机密码库尤其不存在这样的隔离。

若用户需要对恶意 Agent 也保密，须另选受限特权操作代理，禁止任意 root Shell，
将凭据组件与 Agent 可控制的环境分开。这是不同授权模型，不能靠回显隐藏补足。

撤销代填能力不会撤销已获得的 root Shell 或 sudo 缓存。撤权使当前 Agent 控制失效，
界面显示已知特权状态；重新授权前应由用户退出/关闭特权环境，不能假装已经降权。

## 11. 架构与实现单元

数据流：

MCP Service → AccessStore → SessionTerminalController → Parser 输入编码
→ 带来源与操作代际的编码结果 → SessionInputArbiter → 当前 Transport。

当前 Transport → 原 SessionInputPump → 原 TerminalCore
→ TerminalContextProvider → 合并的观察响应。

拆分责任：

- TerminalController：控制权、批次、顺序、抢占及生命周期。
- CommandTracker：只消费已有可信/观测事件，维护命令与就绪证据，不重新获取 Lease。
- ObservationService：组合不可变快照、状态和有界异步等待，不写终端模型。
- ElevationBroker/Adapter：凭据取用和认证隔离，不把秘密交给通用输入链路。
- MCP Service：认证、授权、schema、预算和工具映射，不继续扩大单文件状态机。

状态可以同一 Session 对象内聚合；不为类名另造线程或队列。
合并刷新应由现有 Parser 发布通知驱动，跨线程消息不可变、有容量和停止语义。

## 12. 迁移与交付顺序

新增 terminalControl 和 useElevation 授权字段。
旧 interactiveCommand 带逐条风险确认语义，不能自动升级为完整输入权限。
升级后新复选框默认关闭，由用户明确勾选。旧工具的权限和结果契约保留，
新 Agent 在工具描述中被引导到 read_context/interact_terminal；不混淆执行路径。

旧七工具加两个新工具，共九工具；read_context/list_sessions 扩展 schema。
新版字段纳入输出 schema；旧参数继续有效，回归验证已有客户端对额外字段的处理。
旧 read_context 调用保留原默认值；只有显式 responseMode=compact 或新版 interact
使用低 token 策略。工具说明引导新 Agent 使用该模式，不静默改变旧客户端解析。
不自动修改客户端 MCP 配置、不连接真实服务器或串口做迁移。

交付分三步，各自可构建验证：

1. 输入来源、统一观察/交互、Lease 和等待闭环；一并实现 compact、事件返回、
   Wire/业务等待期限及有界补读。无提示符也能操作终端，不先交付默认高频轮询版本。
2. 命令跟踪和就绪增强，复用既有集成及设备配置；验证证据分级与交互衔接。
3. 托管提权 UI、存储、Broker 和通过验收的 sudo/su 适配器。

第三步依赖真实认证适配器；前两步不能被描述为已经实现密码代填。
托管提权的能力只在该平台/后端适配器验收通过后发布。

## 13. 验收矩阵

默认模拟 Transport、本机回环 HTTP、临时配置，不使用真实凭据或用户目标。

| 场景 | 判据 |
| --- | --- |
| 权限发现/撤销 | 只读零输入；授权后能力可见；撤权和迟到回调零追加写入 |
| 普通命令 | 文本和提交键顺序正确、不额外包装；结果/就绪证据分别返回 |
| 持续滚屏 | 内部最多 4 Hz；attention 不因每批输出返回；完成状态不被后台日志掩盖 |
| 安静长任务 | 等待到期不会报告完成或触发重复输入；可继续观察 |
| y/n、REPL、TUI | 多轮输入生效；命令已结束后拒绝旧关联应答；光标/备用屏准确 |
| 用户抢占/关闭/重连 | 用户优先；世代变化废弃旧令牌；不自动杀远端程序 |
| 精确重试/丢响应 | Transport 写入一次；过期记录不会重新发送；部分提交如实报告 |
| 序号/令牌/容量 | 跨客户端隔离；拒绝冲突、过期、跳号、超大批次和超额等待 |
| 协议自动回复 | DSR/DA 不抢占 Agent，来源不混淆 |
| 文件传输/旧命令 | 与新交互互斥，共用唯一输入通路 |
| 无可信完成证据 | 不捏造 running/exitCode/success；提示符误匹配标 observed |
| 托管认证 | 未授权零取密；伪提示零代填；失败不重试；明文/分片不进入 MCP/历史 |
| 认证隐私与取消 | 旧 capture/排队响应被隔离；异常不自动重开；sudo/su 凭据分别使用 |
| token 成本代理指标 | 普通滚屏合并成一次事件/到期响应；compact 含双份兼容内容的完整结果不超 8 KiB |
| 短命令/长任务 | 30 秒内可信完成只返回一次；安静/有输出但无完成证据都不误报成功 |
| 交互候选与诊断 | 有限检测、指纹去重；同一线索不连发；不确定线索不自动变成应答或命令完成 |
| 裁剪和补读 | projectionLimited 不触发整屏重发；分页同一 capture；过期/源缺口如实报告 |
| 等待期限与取消 | 超过旧 2 秒读取不误超时；新输入可唤醒旧等待；结果生成时刷新合法输入令牌 |

重点运行 MCP 六项、Session、受影响 Core 与 UI 测试，构建主程序。
若按最终实现涉及三个以上模块或公共接口/布局，依 AGENTS.md 跑全套。
Windows 按既有 Qt bin/plugins 前提；模拟通过不替代真实平台/认证适配器验收。
文档阶段只做一致性和格式检查，不声称程序构建、测试或新能力已完成。

参考：docs/ARCHITECTURE.md、P6 实施禁止项、P8 当前记录及当前源码。
认证语义参考：
- https://raw.githubusercontent.com/sudo-project/sudo/main/docs/sudoers.man.in
- https://raw.githubusercontent.com/util-linux/util-linux/master/login-utils/su.1.adoc
