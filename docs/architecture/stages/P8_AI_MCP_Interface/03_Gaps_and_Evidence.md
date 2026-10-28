# 开发调试闭环：现状、缺口与证据

2026-10-28 UART上传的安全文件源缺口已补前置：LocalFileReader同句柄readAt与TransferFileWorker单源工厂，替身核对随机重读、线程/取消和ZSKIP事实。UART独立授权、公开工具、接收完成证据和真实板卡仍未完成，见[记录](records/B3b_Uart_File_Source_2026-10-28.md)。

2026-10-28 G16已补受限SFTP单文件上传、查询及取消的源码通路，复用终端SftpSession；当前二十五工具，实际证据和限制见[上传记录](records/B3b_Sftp_Upload_2026-10-27.md)。下载、覆盖、UART/ZMODEM单文件上传和真机验收仍是明确缺口，不能据此关闭整个G16/B4。

2026-10-27 B3b文件传输第一子批：新增独立本地/远端读写授权与只读 `novaterm_list_file_resources`，当前二十二工具。仅发现已授权元数据，本机路径与文件内容不回传，上传/下载/进度/取消和安全发布仍待后续，见[文件资源记录](records/B3b_File_Resources_2026-10-27.md)。

2026-10-27 B3b第四子批：保存目标支持本机逐客户端的新会话预授权，默认关闭，原创建连接首次就绪后一次绑定读取/完整输入/清空；复用、重试、重连与人工接管不恢复权限。当前二十一工具，单文件传输及B4待后续，见[预授权记录](records/B3b_New_Session_Sharing_2026-10-27.md)。

2026-10-27 B3b第三子批：追加原创建会话安全关闭 `novaterm_close_target`，当前二十一工具；复用/人工接管/旧连接/忙碌及未知状态拒绝，保留可见终端。文件传输、新会话预授权和B4仍待后续，见[安全关闭记录](records/B3b_Target_Close_2026-10-27.md)。

2026-10-27 B3b第二子批：新增独立授权的 `novaterm_open_target` / `novaterm_query_target`，共二十工具；打开/复用使用现有可见终端，配置/客户端身份和operationId绑定，不继承终端权限。关闭、单文件传输与B4仍待后续，见[本批记录](records/B3b_Target_Connections_2026-10-27.md)。

2026-10-27 B3b 第一子批：G17 中「列出明确共享的保存目标」已落地 `novaterm_list_targets`，绑定配置/客户端身份版本且仅当前运行共享，不暴露凭据、不创建连接。打开/复用/查询/关闭与新会话共享规则尚未实施，G16 文件传输及 B4 验收保持待办。精确字段见[当前契约](02_Current_MCP_Contract.md)，范围与命令见[本批记录](records/B3b_Target_Directory_2026-10-27.md)。本页较早静态缺口仍按其日期理解。

2026-10-25 B0/B1 已实施，结果见 [B0/B1 实施与验收](records/B0_B1_Implementation_2026-10-25.md)。原 2026-10-24 静态依据保留；以下 G01/G02/G03/G06/G11/G13 的状态由本页末新增结果更新，真实设备未验收。“存在”指源码/测试用例存在，“缺口”不等于已复现运行故障。实施顺序见 [01](01_Phases_and_Acceptance.md)，游标目标见 [04](04_Cursor_Observation_and_Clear.md)。

## 1. 已有能力，不重复建设

| 能力 | 当前依据 | 边界 |
| --- | --- | --- |
| 本地外部 Agent 接入 | 进程内 Streamable HTTP、Bearer、客户端独立授权；[现行契约](02_Current_MCP_Contract.md) | 应用和目标 Session 必须已存在；工具发现不等于工具可用 |
| 选择 SSH/UART | SessionDirectory、显式 sessionId/epoch、权限与 availability | 不按活动标签隐式选目标，不自动建连 |
| 查看屏幕及有界摘要 | TerminalContextProvider、ObservationService、read_context | 有过滤、去重及截断，不是完整终端日志 |
| 增量观察和等待 | sinceToken/nextToken、ObservationState、attention/change/completion | 已有模型观察游标，不等同于逐行日志读取位置 |
| 按键和文本输入 | interact_terminal 的 text/paste/key/line_ending/command/release | 完整输入需独立授权；command 有就绪约束，通用按键不要求 POSIX Shell |
| 防止重复输入和人工抢占 | inputToken、operationId、replyToExecutionId、SessionInputArbiter | 本机已提交不证明设备收到；人工输入会使旧控制条件失效 |
| 命令和脚本兼容能力 | run_command、run_script、CommandRuntime | 脚本发布限已有 LocalShell/SSH 路径，不是任意固件上传/烧录工具 |
| 独立离线资源读取 | 录制、共享、三个历史工具 | 可选已实现能力，不作为读取实时终端缓存的前置 |

## 2. 必须处理的观察缺口

| ID | 状态与依据 | 对 BSP 的影响 | 收口标准 |
| --- | --- | --- | --- |
| G01 摘要不能充当日志 | **确认的语义差距**。[TerminalStateCache](../../../../src/session/TerminalStateCache.h) 的 append 按文本内容去重，TerminalStateFilter 过滤空白/旋转符；缓存上限来自 TerminalState 的 256 KiB/1024 行 | 相同驱动报错的出现次数和顺序可能消失；加大返回预算无法恢复被过滤的内容 | 日志读取以终端历史的稳定位置为准，保留真实重复行；重复读同一位置可以识别，不能按内容去重 |
| G02 两类游标混用 | **确认的契约差距**。[TerminalInteraction](../../../../src/mcp/TerminalInteraction.cpp) observation 先生成基于模型 revision 的 nextToken，fit 另生成固定 capture 的 expandToken；full 截断时清空 nextToken。现有测试 fullProjectionTruncationRequiresReset 明确锁定旧行为 | 不能把 nextToken 当作“所有此前日志已经交付”；固定 capture 补读也不能覆盖之后的新输出 | 日志游标按实际交付位置前进；页未读完时可继续，结束后继续实时读取；旧观察行为兼容保留 |
| G03 有限摘要与终端缓存范围不同 | **确认的范围限制**。[TerminalContextProvider](../../../../src/session/TerminalContextProvider.h) 只维护有界摘要；search_context 搜索短期 capture，当次投影之外不可查 | Agent 编译期间未读，回来时早期启动输出可能还在终端历史中，却不在摘要中 | 明确 oldest/tail/增量范围和可读边界；按需分页读取现有 Scrollback，缓存淘汰后报告缺口，不承诺无限保留 |
| G04 清空缺少 MCP 事务 | **确认未开放**。[Core](../../../../src/core/terminal/TerminalCore.cpp) 有 ClearScrollback/ClearAll 的 Parser 命令；[UI](../../../../src/ui/terminal/TerminalView.cpp) 调用它们，当前工具目录无清空工具 | Agent 不能请求本地清空；命令成功入队也不等于清空完成 | 明确授权、范围、一次操作 ID、Parser 完成边界和新代际；清空后旧日志游标失效，重试不再次清除新输出 |
| G05 清空后的摘要失效 | **待专项核验**。Provider 的 acceptState 显式处理 alternate-screen 切换和 revision 回退；本轮未确认普通 ClearAll 到 Provider 缓存、capture、游标的完整失效链 | 清屏成功后，Agent 可能仍通过旧观察状态看到先前文本；不能只验证窗口已空 | Core 清空到 Session、历史读取和观察投影形成一致边界；全链路测试，包括本机 UI 发起清空 |
| G06 实时屏幕与日志语义分离 | **已有部分处理，尚需场景验收**。Provider 不累积光标行的 CR 改写，alternate screen 不提供普通 recentOutput | U-Boot 倒计时、登录提示、进度条、top 等不一定产生追加行 | 日志页不伪造屏幕版本为新日志；当前屏幕仍可读，明确 partial/快照时间/刷新语义 |

G01～G03 是旧“有界摘要”目标与新“可靠读取终端缓存”目标的差异，不将旧行为直接定性为回归。G05 未做运行复现，不写成已确认漏洞。

## 3. 操作闭环的遗留边界

| ID | 事实与证据 | 下一步 |
| --- | --- | --- |
| G07 UART 静默不是成功 | [SessionCommandCoordinator](../../../../src/session/SessionCommandCoordinator.cpp) 的 allowIdleCompletion 以静默/无响应结束；[测试](../../../../tests/mcp/McpTests.cpp) serialWithoutPromptCompletesByQuietWindow 覆盖该降级。现行结果无退出码，并提示未验证；terminationConfirmed 在该兼容路径为 true，不能单独当作远端进程终止证据 | 使用说明和输出证据等级一致：停止等待、完成命令、测试成功是三件事；对迟发错误、静默长任务验收，不靠闲置窗判成功 |
| G08 SSH 未知输入位置 | 旧 run_command 允许无可信提示符的 SSH/POSIX 完成标记路径；[测试](../../../../tests/mcp/McpTests.cpp) unverifiedSshCommandUsesCurrentTerminal。结束标记不证明提交前处于空闲 Shell | 优先观察并使用带 inputToken/就绪约束的输入；核验 TUI、密码提示、未提交命令、嵌套 Shell，不盲发探测 Enter/Ctrl+C |
| G09 断连与设备重启 | 现有授权/epoch 处理重连失效；但板子重启可能保持 UART 链路，同一 Session 可能从 Linux 转入 U-Boot | 分别测试链路重连、同链路重启和环境切换。SSH/UART 由用户明确配对，Agent 核对启动/固件标志；不能按标题或相同时间自动认定同一板子 |
| G10 长任务与取消 | 单次 wait 最大 30 秒，已有持续等待、取消、unknown/目标保护和人工接管机制 | 长时观察采用多次有界等待；验证停止等待不会误发 Ctrl+C，取消命令不保证远端停止，不换 operationId 盲重放 |
| G11 Agent 使用说明冲突 | [WireSession](../../../../src/mcp/WireSession.cpp) 两代握手 instructions 仍分别包含“非严格只读模板均需确认”和“仅单独授权固定诊断可执行”，与已有完整输入/普通命令路径不完全一致；工具说明已有观察建议 | 统一两代握手、工具描述和用户说明，覆盖日志书签、屏幕、缺口、清空和 UART 静默；用契约测试防止指令互相矛盾 |
| G12 真机与性能证据不足 | 既有用例包含 SSH/Serial 替身和输入/授权回归，但不等于完整硬件闭环。串口压力历史记录显示驱动 RX 队列设置会影响丢帧 | 单独完成真实 SSH、UART 启动、双通道重启、缓存淘汰及高输出验收；区分设备/驱动丢字节、终端缓存淘汰和 MCP 投影截断 |
| G13 工作区与发布基线不一致 | [P4 暂停记录](records/P4_Implementation_and_Verification.md) 记载未完成回归及串口完成标记失败；源码有两项诊断扩展，旧冻结契约为九终端工具加三历史工具 | 后续实施前固定基线并明确保留/隔离策略。此次文档删除不等于扩展已经从构建移除或已经发布 |

## 4. Netcatty 对照补充（2026-10-25）

以下来自此前对本机 E:/code/Qt/Netcatty 的只读源码比较，不是运行验收。范围已纳入 [06 补齐设计](06_Terminal_Workflow_Extensions.md)，对应 B3a/B3b；不要求第一批日志功能等待所有扩展完成。

| ID | 对照依据与 NovaTerm 差距 | 补齐与验收 |
| --- | --- | --- |
| G14 条件等待 | Netcatty 的 electron/scripts/sessionOutputBuffer.cjs 提供 waitForText/Regex/Prompt，经 scripts_run 使用；NovaTerm read_context 当前只有 attention/change/completion，指定文本需调用方反复读取判断 | 复用缓存和屏幕的有界字面等待；旧文本、跨页匹配、扫描续接、清空/淘汰和取消都有明确语义，不引入脚本引擎 |
| G15 长任务入口 | Netcatty terminal_start/poll/stop 返回任务状态、nextOffset/outputBaseOffset；其 execHandlers 明确拒绝 Serial 后台执行。NovaTerm 有执行状态和持续观察，但没有同样直接的任务查询接口 | 薄异步入口复用 CommandRuntime/Controller；SSH/UART 分别表达提交、完成证据和可停止能力；不能把 UART 静默当 exitCode=0 |
| G16 文件交付 | Netcatty catalog/sftp.cjs 暴露单文件上传/下载等工具；NovaTerm 有传输/脚本能力，但当前 MCP run_script 不是任意二进制文件传输 | 单文件 SSH/SFTP、授权本地资源引用、覆盖策略、进度/取消及发布事实；不自动烧录，不开放任意本机路径 |
| G17 目标连接 | Netcatty host_open 使用保存目标，优先复用连接，session_close 区分创建与复用的所有者；NovaTerm 当前 MCP 只能操作已存在的共享会话 | 只列和打开获准保存目标，配置版本/operationId 绑定，busy/认证待处理明确返回；不关闭复用的人类会话 |

G01～G03 对照补充：Netcatty components/Terminal.tsx 和 domain/terminalContextRead.ts 直接按 viewport/tail/head/lines 读取缓存，默认 80 行、上限 300 行；这比有界去重摘要更适于回查，但仍不是带清空代际的连续日志游标。NovaTerm 按 04 补稳定位置，不复制可随缓存淘汰移动的裸行号。

G11 对照补充：Netcatty schemas/toolInputs.cjs 有发现会话、短/长任务选择和 Shell 状态说明。NovaTerm 应统一指引，但保留当前交互终端 cwd/env 语义，不照搬 Netcatty 隔离子 Shell 的命令模式。清空与代际失效仍是本项目目标，未确认 Netcatty 已通过等价闭环验收。

## 5. 不作为首个闭环前置的能力

- 本地编码、编译、工程管理、部署/烧录编排由外部 Agent 和现有工具负责，不在 NovaTerm 新建平台。
- SSH/SFTP 单文件传输按 B3b 补齐，已有部署脚本不因此废弃；串口文件传输、递归文件管理和自动烧录不作为这次扩展目标。
- 不要求新增持久录制、全文索引、诊断 Pack、自动分析器、证据报告平台。
- 无可信适配器的托管提权继续 unavailable，不以自动提权作为 SSH/UART 观察前置。
- MCP 不保证毫秒级 U-Boot 抢键或远程电源/JTAG 控制；短倒计时需先实测，无法满足时明确由用户或已有独立工具处理。

## 6. 证据使用规则

测试名称表示已存在对应断言，不代表本轮执行通过。源码静态差距、替身测试、真实设备、跨平台和性能分别记录；不以过去全套成绩代替本次验收。具体工作项与验收映射见 [01](01_Phases_and_Acceptance.md)。


## 7. B0/B1 更新（2026-10-25）

- G01/G02/G03：已实现 source=log 的已提交历史位置、真实重复行、oldest/tail、UTF-8 分页、缓存淘汰和冷页状态；旧摘要/模型游标保持原义。准确协议在 02，不用旧表的“确认差距”推断当前仍缺实现。
- G06：两种来源显式分离，活动屏幕、CR/未换行与备用屏仍用 observation；日志仅消费提交历史。替身/Core 回归见本轮记录，真实设备仍待验收。
- G11：两代握手共用同一 instructions，说明完整输入广泛授权、UART 静默证据、未知重放及未实现工具；增加一致性测试。
- G13：未发布 P4 两工具及专属实现已退役，P1 无消费者的重复执行/环境/本机 Agent 原型一起删除；工具目录恢复十二项并只演进 read_context 的显式日志分支。
- G04/G05：MCP 清空事务与旧 observation/capture 清空失效继续留在 B2。本轮后端日志代际已响应本机 clear/pop，但不因此把 G05 标为解决。
- G07～G10、G12、G14～G17：既有边界/未来批次保持；本轮没有新增等待/任务/传输/建连工具，没有真机闭环或性能验收结论。

## B2 实施更新（2026-10-25）

G04/G05 已通过独立清空权限、现有 Parser 队列事务和统一缓存代际接入；上表旧静态盘点保留为历史依据。当前十三工具、十分钟票据与有界幂等、旧观察/capture/token 的失效规则见 02。具体本轮命令、回归、审查修复及未验收范围见 [B2 记录](records/B2_Implementation_2026-10-25.md)。真实设备闭环与性能 G12 仍未验收。

## B3/B3a 实施更新（2026-10-27）

- G07/G10/G14/G15：新增字面文本等待及长任务三工具，当前共十七项。日志/屏幕起点、固定期限续扫、原执行owner、未知结果与UART静默证据已在02冻结；真实设备仍另验。
- G08/G09：保留command就绪和原始输入分工；补Ctrl+C后的可信prompt恢复、SSH/UART行尾/按键矩阵、可信ShellReset/FramingError使旧控制/停止失效。普通启动文本不猜新epoch，固件轮次由调用方核对。
- G11：两代握手复用同一instructions，工具说明、使用文档与短/长任务流程统一；MCP文件/保存目标连接未实现不推荐调用。
- 新增独立审查修复包括服务重新启用任务owner、可信环境重置后的旧停止、断流等待配额回收、失效响应Schema与非ok日志页前缀扫描。实际RED/GREEN与最终成绩见 [B3记录](records/B3_Implementation_2026-10-27.md)。
- G12、G16/G17仍未验收/实施；不从替身测试推断真实SSH/UART、性能、跨平台或完整BSP闭环完成。
