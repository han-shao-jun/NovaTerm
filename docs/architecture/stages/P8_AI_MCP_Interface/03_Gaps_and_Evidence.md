# 开发调试闭环：现状、缺口与证据

更新：2026-10-31。当前二十工具；Windows B0～B3b基础能力包括SFTP上传/下载及UART发送，SFTP显式覆盖本阶段Windows软件回归已完成，B4仍待用户验收，见[覆盖记录](records/B3b_File_Overwrite_2026-10-28.md)。保持原FileTransfers/SftpSession/worker与本地安全后端，不新增owner。

G16本轮Windows显式覆盖与软件收尾已完成Windows软件验证；Linux安全读取源后端已实现；Linux安全Writer于2026-10-31实现并通过软件回归，实际设备下载仍待验收；macOS文件后端未实现，属于开发缺口。G12/B4真实板卡、客户端和性能由用户验收，P8未整体完成。覆盖只授权可信目录的目标路径替换，非CAS；本地pinPath没有ACL/owner检查。托管提权继续unavailable，非当前闭环前置。

以下带日期的静态盘点及阶段更新用于追溯，现行能力以[02](02_Current_MCP_Contract.md)与源码为准。历史结果见[验收档案](records/README.md)，后续任务见[软件收口账本](08_Software_Closeout.md)；“存在”只表示源码/用例存在，不表示本阶段测试已通过。

## 1. 已有能力，不重复建设

| 能力 | 当前依据 | 边界 |
| --- | --- | --- |
| 本地外部 Agent 接入 | 进程内 Streamable HTTP、统一Bearer与四项会话授权；[现行契约](02_Current_MCP_Contract.md) | 应用和目标 Session 必须已存在；工具发现不等于工具可用 |
| 选择 SSH/UART | SessionDirectory、显式 sessionId/epoch、权限与 availability | 不按活动标签隐式选目标，不自动建连 |
| 查看屏幕及有界摘要 | TerminalContextProvider、ObservationService、read_context | 有过滤、去重及截断，不是完整终端日志 |
| 增量观察和等待 | sinceToken/nextToken、ObservationState、attention/change/completion | 已有模型观察游标，不等同于逐行日志读取位置 |
| 按键和文本输入 | interact_terminal 的 text/paste/key/line_ending/command/release | 完整输入需独立授权；command 有就绪约束，通用按键不要求 POSIX Shell |
| 防止重复输入和人工抢占 | inputToken、operationId、replyToExecutionId、SessionInputArbiter | 本机已提交不证明设备收到；人工输入会使旧控制条件失效 |
| 终端命令与任务 | interact_terminal、start_task/query_task/stop_task、SessionTerminalController | 复用当前终端；旧诊断、命令、脚本工具及其专属执行器、发布后端已删除 |
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
| G07 UART 静默不是成功 | 旧Coordinator的allowIdleCompletion命令执行分支与对应测试已删除；Coordinator仅维护提示符证据，现行Controller/任务仍按可信事件区分完成与未知 | 对迟发错误、静默长任务继续做设备验收，不把观察到静默当成退出或成功 |
| G08 SSH 未知输入位置 | 旧run_command及无可信提示符的完成标记执行路径已删除；当前command动作要求就绪，原始输入仍可能作用于TUI、密码提示或未提交命令 | 输入前观察目标与状态；核验TUI、嵌套Shell，不盲发探测Enter/Ctrl+C |
| G09 断连与设备重启 | 现有授权/epoch 处理重连失效；但板子重启可能保持 UART 链路，同一 Session 可能从 Linux 转入 U-Boot | 分别测试链路重连、同链路重启和环境切换。SSH/UART 由用户明确配对，Agent 核对启动/固件标志；不能按标题或相同时间自动认定同一板子 |
| G10 长任务与取消 | 单次 wait 最大 30 秒，已有持续等待、取消、unknown/目标保护和人工接管机制 | 长时观察采用多次有界等待；验证停止等待不会误发 Ctrl+C，取消命令不保证远端停止，不换 operationId 盲重放 |
| G11 Agent 使用说明一致性 | 两代握手共用instructions，旧诊断允许列表及逐命令确认不再是现行授权；旧确认状态只返回拒绝，不进入业务层 | 保持工具描述、使用说明与当前四项授权一致；真实客户端提示和审批仍单独验收 |
| G12 真机与性能证据不足 | 既有用例包含 SSH/Serial 替身和输入/授权回归，但不等于完整硬件闭环。串口压力历史记录显示驱动 RX 队列设置会影响丢帧 | 单独完成真实 SSH、UART 启动、双通道重启、缓存淘汰及高输出验收；区分设备/驱动丢字节、终端缓存淘汰和 MCP 投影截断 |
| G13 工作区与发布基线 | 旧诊断helper、Facade/Executor、ScriptProvider、旧权限及SFTP脚本字节发布接口已从源码和构建接线删除；旧冻结fixture仅保留历史对照 | 当前源码变更不等于发布或B4验收；构建、测试与设备证据分别核实 |

## 4. Netcatty 对照补充（2026-10-25）

以下来自此前对本机 E:/code/Qt/Netcatty 的只读源码比较，不是运行验收。范围已纳入 [06 补齐设计](06_Terminal_Workflow_Extensions.md)，对应 B3a/B3b；不要求第一批日志功能等待所有扩展完成。

| ID | 对照依据与 NovaTerm 差距 | 补齐与验收 |
| --- | --- | --- |
| G14 条件等待 | Netcatty 的 electron/scripts/sessionOutputBuffer.cjs 提供 waitForText/Regex/Prompt，经 scripts_run 使用；NovaTerm read_context 当前只有 attention/change/completion，指定文本需调用方反复读取判断 | 复用缓存和屏幕的有界字面等待；旧文本、跨页匹配、扫描续接、清空/淘汰和取消都有明确语义，不引入脚本引擎 |
| G15 长任务入口 | Netcatty提供terminal_start/poll/stop；NovaTerm现已有start_task/query_task/stop_task，复用TerminalTasks与Controller，不依赖旧CommandRuntime | SSH/UART分别表达提交、完成证据和可停止能力；不能把UART静默当exitCode=0，真实设备仍待验收 |
| G16 文件交付 | 当前MCP提供SFTP上传/下载与UART发送，复用已有owner及安全文件后端；旧run_script与专属脚本发布接口已删除 | 按现行路径确认、覆盖、进度/取消及发布事实验收；上传不自动执行或烧录 |
| G17 目标连接 | 当前MCP只操作已连接会话；保存目标四工具和初始化授权接口已删除 | 用户先建立连接，Agent核对sessionId/epoch与真实目标；不恢复自动打开或关闭保存目标的旧工具 |

G01～G03 对照补充：Netcatty components/Terminal.tsx 和 domain/terminalContextRead.ts 直接按 viewport/tail/head/lines 读取缓存，默认 80 行、上限 300 行；这比有界去重摘要更适于回查，但仍不是带清空代际的连续日志游标。NovaTerm 按 04 补稳定位置，不复制可随缓存淘汰移动的裸行号。

G11 对照补充：Netcatty schemas/toolInputs.cjs 有发现会话、短/长任务选择和 Shell 状态说明。NovaTerm 应统一指引，但保留当前交互终端 cwd/env 语义，不照搬 Netcatty 隔离子 Shell 的命令模式。清空与代际失效仍是本项目目标，未确认 Netcatty 已通过等价闭环验收。

## 5. 不作为首个闭环前置的能力

- 本地编码、编译、工程管理、部署/烧录编排由外部 Agent 和现有工具负责，不在 NovaTerm 新建平台。
- SSH/SFTP上传/下载与UART/ZMODEM单文件发送已纳入B3b，已有部署脚本保持独立。MCP串口文件接收、递归文件管理、新增协议及自动烧录不属于当前软件闭环范围。
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
