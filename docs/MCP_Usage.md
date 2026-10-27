# NovaTerm AI MCP 使用说明

2026-10-27：新增 `novaterm_close_target`，当前二十一工具；仅关闭原操作创建且未人工接管、具有空闲证据的连接，保留终端。单文件传输和B4待后续，见[安全关闭记录](architecture/stages/P8_AI_MCP_Interface/records/B3b_Target_Close_2026-10-27.md)。

历史MCP测试、审查与缺口详见[实施与验收记录](architecture/stages/P8_AI_MCP_Interface/records/03_Implementation_and_Verification.md)；引用成绩须按日期与平台区分。

NovaTerm 提供进程内 Streamable HTTP MCP 服务，可让支持 MCP 的 AI 客户端读取指定终端输出、
搜索已读取的内容、运行授权诊断，以及通过当前交互终端执行受控命令和脚本任务。默认关闭。

历史录制与读取另行授权：在主菜单 **历史录制** 中手动开始/停止录制，选择录制条目和已登记的 MCP 客户端，填写 record 范围后点击 **Share selected**。终端的「读取输出」权限不会授权完整历史。范围终点为 `0` 表示每次请求时的已提交高水位，活动录制后续提交的数据也在此选择范围内；指定有限 record ID 可固定共享范围。默认共享到退出，勾选 **Keep permission after restart** 才保存离线权限；关闭终端不会撤销，撤回请用 **Revoke history permission**。

历史工具为 `novaterm_list_captures`（仅列获准录制）、`novaterm_read_history`（base64 原文字节分页）、`novaterm_search_history`（UTF-8 文本或原文字节 literal 搜索）。按返回的 nextCursor 续页，不把 `complete=false` 或 unknown_tail 当成无缺失/无故障。搜索证据默认仅保留期限内存，退出后不可恢复；历史工具不会自动启用录制、恢复设备连接或执行命令。本机 **Share local log file…** 只共享明确选择的文件资源，最多 16 MiB，权限到退出，可用 **Revoke imported files** 撤回；本轮不规划离线分析器。

## 开始使用

1. 编译并运行 NovaTerm，Qt SDK 需要包含匹配版本的 **HttpServer** 组件。
   MCP 服务由主程序持有，不需要独立 MCP 桥接进程。
2. 在主菜单打开 **AI MCP 接入**，勾选 **启用本机 MCP 接入**。
3. 输入客户端名称，点击 **添加客户端**。
4. 在会话列表勾选需要共享的 **读取输出**。新建会话和应用重启后的会话默认不共享。
   如需让 Agent 连续操作当前终端，再明确勾选 **允许执行命令**。它授予文本、按键、
   粘贴和命令输入权限，之后不逐条确认；原有交互命令授权不会自动升级为此权限。
5. 点击 **复制 MCP 配置**，粘贴到所使用的 MCP 客户端配置中。配置含接入令牌，
   不要公开、提交到仓库或发送给其他人。
6. 使用 CC Switch 时点击 **复制 CC Switch 配置**，把单个 server 对象粘贴到
   「完整的 JSON 配置」编辑器，并将唯一标题设为 `novaterm`。不要额外包裹
   `mcpServers` 或 `novaterm`；CC Switch 会用标题作为服务器 ID。

只有已经打开且明确授予终端权限的会话可见。MCP不自动启动NovaTerm；本机用户另行允许保存目标打开后，可按下文创建可见SSH/UART终端。切换当前标签不会改变工具的目标，调用按 sessionId 和 epoch 定位。

默认接入地址为 `http://127.0.0.1:38973/mcp`，应用重启后保持稳定；应用启动前
可用环境变量 `NOVATERM_MCP_HTTP_PORT` 覆盖端口。端口占用会明确报错，不自动
换端口。需要多个应用实例时为各实例显式选择不同端口，使用各自导出的 URL 和令牌。
应用必须运行且已启用 MCP，客户端不会自动启动主程序。重启后仍需重新选择共享会话。

导出的单个 HTTP server 对象形状如下，令牌仅为占位值：

```json
{
  "type": "http",
  "url": "http://127.0.0.1:38973/mcp",
  "headers": {"Authorization": "Bearer <从 NovaTerm 导出的接入令牌>"}
}
```

Codex 的 TOML 配置使用以下字段：

```toml
[mcp_servers.novaterm]
url = "http://127.0.0.1:38973/mcp"
http_headers = { Authorization = "Bearer <从 NovaTerm 导出的接入令牌>" }
```

旧 `command`、`args`、`env` 及 `--instance` 配置需要替换成 HTTP 配置；
`novaterm-mcp` 已删除，不保留 stdio 兼容入口。每个请求都需要 Bearer token，
`MCP-Session-Id` 或客户端名称不能替代认证。只监听本机回环地址。

## 九个兼容终端工具

| 工具 | 用途 |
| --- | --- |
| `novaterm_list_sessions` | 列出该客户端获准访问的会话及身份 |
| `novaterm_read_context` | 缺省读取屏幕/摘要；source=log 按精确位置读取保留的终端历史 |
| `novaterm_search_context` | 搜索一次已返回的 capture，不搜索整个历史或执行正则 |
| `novaterm_interact_terminal` | 按观察返回的输入令牌操作当前终端，并等待下一次有用反馈 |
| `novaterm_request_elevation` | 请求托管 sudo/su；当前没有可信认证适配器，返回不可用且不读取密码、不输入命令 |
| `novaterm_list_commands` | 查看当前获准的固定诊断模板与短期执行票据 |
| `novaterm_execute_command` | 用票据执行对应模板；不接受 shell 文本 |
| `novaterm_run_command` | 在明确共享的当前交互终端执行命令；高风险或无法分类的命令需要 MCP 客户端人工确认 |
| `novaterm_run_script` | 确认后将脚本写入 LocalShell/SSH 主机的指定路径，再在当前交互终端显示并执行调用命令 |

缺省观察上下文是摘要：重复行和进度噪声可能被过滤，长输出可能截断；备用屏
主要返回当前屏幕。必须检查 `sourceTruncated`、`outputTruncated`、`resetRequired`。
旧读取在截断时不返回可推进的 nextToken。compact 读取另外区分 `projectionLimited`
（本次预算省略，可用 `expandToken` 补读）和 `sourceGap`（源摘要已有缺口，不能补齐）；
其 nextToken 可以继续观察新事件，不能据此声称已读完全部日志。

终端中已经显示的敏感信息可能随授权输出被读取。程序不提供获取会话密码、私钥
或凭据引用的 MCP 工具，但也不会声称可以自动识别终端里所有敏感文字。

## 按位置读取日志（B1）

日志与屏幕分开读取。先用 `novaterm_read_context` 保存当前已提交历史的尾书签：

```json
{"sessionId":"<session id>","epoch":"<current epoch>","source":"log","start":"tail"}
```

要回查仍保留的旧文本，将 start 改为 oldest。后续传日志响应的 nextToken：

```json
{"sessionId":"<session id>","epoch":"<current epoch>","source":"log","sinceToken":"<log nextToken>","maxBytes":65536,"maxLines":256}
```

串接 fragments[].text，按 lineId/byteOffset 识别重试重叠，保留不同位置的相同文本。lineComplete 表示该历史行已交付完；hardBreak=true 的末段含 LF，重连后遗留的软行 hardBreak=false，不补 LF。hasMore 时续页，caughtUp 只表示追上本次采样上界，后续输出继续沿 nextToken 读取。

status=gap 表示旧位置被淘汰；status=invalidated 表示本机 clear 或 resize 回填改变了缓存代际。两者都不自动换起点，明确记录不完整后用 earliestToken（保留起点）或 sampledEndToken（当前尾书签）重新建立基线。pending/busy 表示冷页尚未就绪/资源忙，保留已返回片段并沿 nextToken 稍后重试；unavailable 如实报告源不可用。Core 锁忙会返回 BUSY，仍用原请求重试。TEXT_BUDGET_TOO_SMALL 要增大 maxBytes。

预算缺省 65536 字节/256 分段，最大 262144/1024；compact 只改缺省为 4096/24。初次 start 与续读 sinceToken 必须二选一；日志模式不要传 expandToken、waitFor、非零 waitMs 或 includeViewport=true。只有读取授权也可建立书签，整个过程没有输入或清空。

当前软尾行、未换行提示、CR 进度条和备用屏仍看缺省 observation。日志只读已稳定的终端缓存，没有原始字节或覆写前屏幕帧；分页完整不证明 UART 硬件零丢失。日志 token 与观察 token/capture/expandToken 不混用，不产生 inputToken；需要操作时另读屏幕。MCP 清空按下节独立授权开放；文本等待和长任务见下文，保存目标打开/查询/安全关闭见下节；文件传输工具尚未开放。

## 文本等待与长任务（B3/B3a）

当前共二十一个工具：上述九项、三项离线历史、独立清空、以下四项等待/任务，再加保存目标目录、打开、查询和安全关闭四项。SSH/UART交互均复用当前可见终端；先列会话核对 availability.terminalTasks，再观察和输入。

保存目标目录需要在「AI MCP access」中选择客户端，勾选「保存目标」的「共享目录」列；仅当前应用运行有效。`novaterm_list_targets` 可用 limit（默认25，最多50）及返回的 nextCursor 分页；配置修改、重新保存或共享变化后续页返回 TARGET_DIRECTORY_CHANGED，应重新列举。它仅披露所选 SSH/UART 的名称和端点摘要，saved_configuration 不表示设备在线；不连接设备，不授予终端读取/输入或文件权限。打开还需独立授权，见下文；安全关闭见下文；MCP单文件传输仍未提供。

需要Agent打开保存目标时，在该客户端下先勾「共享目录」，再单独勾「允许打开」。调用 `novaterm_open_target`，只传目录返回的 `targetId`、`configVersion` 和新的UUID `operationId`；不能修改地址、串口参数、认证或传密码。匹配的连接会复用，否则创建可见标签；串口被其他配置占用时返回 `state=unavailable, reason=port_busy`，不会关闭人的会话来腾端口。本机手动断开已停止自动重连、且端口实际释放后，旧标签不会继续占用预留；真正待重连或待释放的端口仍受保护。

保存原参数，用 `novaterm_query_target` 查询connecting/running/failed等实际状态。首次连接和同目标重连可更新epoch；换绑目标返回invalidated。`reason=host_key_confirmation_required` 时由本机用户处理主机密钥窗口；`authentication_required` 时在本机编辑凭据。不要把connecting、accepted或已建标签当作登录成功。

打开不授予终端读取、输入、清空或文件权限。新标签需本机再勾相应会话权限，Agent随后用list_sessions与观察工具取得有效sessionId/epoch/inputToken。撤回目录共享同时撤回允许打开，重新共享不会恢复打开权。

同一操作只用原operationId和完全相同参数重试，查询不会重连或重开标签。运行期每客户端最多32、全局256条打开操作，满额拒绝新操作但保留已有查询；记录不跨重启恢复。失败/缺记录/未知结果都不应换ID自动重跑。收尾可调用 `novaterm_close_target`，传原targetId/configVersion/operationId及最近查询epoch。仅原操作创建、未被人工终端输入或本机手动连接动作接管、仍为原连接且有可靠空闲证据时关闭。复用的人类会话拒绝；忙碌或未知提示符的UART返回TARGET_CLOSE_BUSY，不强关。成功返回closing/closed，保留可见终端和缓存；关闭精确重试使用相同epoch，不再次关闭。初次连接指纹发现可使用最新epoch；重新连接后的连接不由旧打开操作关闭。文件操作/执行/认证待处理时等待或由本机用户收尾。


目录名称采用保存标签，空标签回退主机名或串口名，不包含 SSH 登录用户名。给同一客户端的活动 Session 修改读取/输入等权限，不会清掉独立的目录共享；令牌轮换、删除客户端和全局停用仍会撤销目录共享。

| 工具 | 选择方式 |
| --- | --- |
| novaterm_wait_text | 仅读取授权；source=log从sinceToken之后扫描，省略则从本次尾书签开始；source=screen必须start=current或next，用于未换行提示/TUI |
| novaterm_start_task | 完整输入授权、最新inputToken、新operationId、actions；command要求就绪，text/key/line_ending等支持设备CLI；保存taskId |
| novaterm_query_task | 按taskId或原启动operationId查询，不再输入；可选B1日志sinceToken读取新增输出 |
| novaterm_stop_task | taskId和独立停止operationId明确请求一次Ctrl+C；原控制/执行必须仍有效，人工已接管时零写入拒绝 |

日志等待示例（只匹配书签后的新文本，旧PASS不触发）：

```json
{"sessionId":"<id>","epoch":"<epoch>","source":"log","sinceToken":"<B1 log nextToken>","texts":["TEST_PASS","TEST_FAIL"],"match":"any","waitMs":30000}
```

省略sinceToken意味着从首次成功采样的尾部建立基线；要等待当前已显示的login提示，显式使用source=screen、start=current。start=next先建立屏幕revision基线，不拼接不同屏幕。文本最多8项，每项256 UTF-8字节；any/all和大小写可选，无正则；all不证明顺序，屏幕all须在同一份快照成立。

日志尾部仅指已提交历史；仍在活动屏幕的旧文字随后滚入历史时，也可能被该基线之后的日志等待命中。按测试轮次判断时结合独有标志及实际固件身份，不能只凭通用PASS推断新一轮已经执行。普通交互用过的operationId也不能配合新inputToken改作新任务启动；新意图使用新ID，同一任务的网络重试才复用原ID和原参数。

单次等待最长30秒、默认扫描256KiB/最大1MiB。timeout或scan_budget_exhausted返回resumeToken，重传同一匹配条件继续（不再带sinceToken）；可以调整waitMs/scanBytes。续扫状态固定十分钟，每客户端8/全局32，过期明确拒绝。nextToken是服务端扫描进度；需要完整日志另从startToken分页，不把命中上下文当作已读全文。gap/invalidated须承认缺口并显式选新基线；pending/busy冷页等待不证明没有文本。取消等待不发送任何输入。

长任务启动示例：

```json
{"sessionId":"<id>","epoch":"<epoch>","inputToken":"<fresh observation>","operationId":"<new UUID>","actions":[{"type":"command","command":"make test"}],"timeoutMs":600000}
```

启动只做一次；短RPC断流不停止已受理任务。用taskId查询，若启动响应丢失，用原operationId查询受理；网络重试使用相同ID和全部相同参数，不换ID重输。任务每客户端32/全局128、终态保留十分钟；应用重启或关闭MCP不恢复执行索引，记录丢失时报告未知，不能自动重跑。

分别检查status、submissionStatus、completed、evidenceKind、exitCode和terminationConfirmed。UART原始任务静默仍running，执行期限到达unknown，不能捏造exitCode=0；SSH提示符也不证明功能测试成功。停止请求与远端确认终止分别解释；人工接管、可信环境reset、断连或新执行后旧停止请求不再有效。任务输出仍来自终端缓存，清空/淘汰可造成output缺口，但不抹除执行事实。

构建、部署、固件标识和功能判据由外部Agent分别确认；文本命中只证明文本出现。完整字段、预算和验收边界见 [现行契约](architecture/stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md)。

## 本地清空（B2）

默认不允许 MCP 清空。在本机 MCP 设置中为指定客户端/会话单独勾选清空权限后，先读取一次日志页，再显式调用 `novaterm_clear_terminal`：

```json
{"sessionId":"<session id>","epoch":"<current epoch>","operationId":"<new UUID>","scope":"scrollback","expectedGeneration":"<log cacheGeneration>","clearToken":"<log clearToken>"}
```

scope=all 同时清屏与历史；scrollback 仅清历史。成功表示 Parser 已完成，响应 baseline 是该执行点的新日志书签，不保证新输出尚未到达。相同 operationId 只可连同全部原参数精确重试，返回原结果且不再清空。clearToken 有效十分钟；过期/撤权/会话换代/人工接管要明确处理，未知结果不能换票据或 ID 自动重放。清空会使旧观察、日志游标与实时 capture/expand/inputToken 失效，须取新基线。

清空不会向设备发送字节、停止远端程序、断开连接或删除录制/离线共享。独立清空授权不会授予终端输入，旧完整输入授权也不会自动获得清空权限。

## 终端观察与操作

新客户端优先使用 `read_context` / `interact_terminal` 闭环。先读取目标的
`permissions` 和 `availability`，再用 compact 观察返回的 `inputToken` 提交操作：

```json
{
  "sessionId": "<session id>",
  "epoch": "<current epoch>",
  "responseMode": "compact"
}
```

将其 `data.inputToken` 用于 `novaterm_interact_terminal`；例如在已就绪的命令行运行 pwd：

```json
{
  "sessionId": "<session id>",
  "epoch": "<current epoch>",
  "inputToken": "<from observation>",
  "operationId": "<new UUID>",
  "actions": [{"type": "command", "command": "pwd"}]
}
```

`command` 要求提示符就绪；没有提示符时可基于屏幕用 `text`、`key`、`paste` 或
`line_ending` 操作 REPL、TUI 和设备 CLI。一次最多 64 动作、原文合计 16 KiB，
编码字节最多 64 KiB。`release` 必须独立调用，只释放输入权。用户输入或本机
“接管选中会话”可打断控制；重连、撤权和用户输入后需重新观察，旧令牌拒绝使用。

这套基础闭环适用于 LocalShell、SSH、Serial、Telnet 和 Custom；不会仅因串口或
自定义连接而禁用输入。Shell/提示符规则描述对端环境，可显式跨传输配置；不自动
向未知设备发送探测或安装 hook。切换连接类型时，简写 attach 不继承旧端点规则。

命令默认等待 `attention` 最多 30 秒；按键/文本默认等待 `change` 最多 1 秒。
仍在运行时，用 `nextToken` 作为下一次读取的 `sinceToken`，并设置
`responseMode=compact`、`waitFor=attention`、`waitMs=30000` 继续等待。
`completion` 只等待完成或必须处理的控制事件；`waitMs=0` 可立即读取。
工具调用到期不会终止远端程序，也不能推断命令成功。检查 `command` 的证据、
退出码和 `inputReadiness.canSubmitCommand`；活动命令的追加输入要带
`replyToExecutionId`，避免将迟到应答写入新的命令行。

普通输出的基础快照最多每 250 ms 更新一次，同一会话的多个等待/读取复用它；
控制和命令状态独立更新。`capturedAt` 是屏幕采样时间，快速连续读取可能得到同一
屏幕而控制状态已经变化；需要后续输出时继续增量读取，不把一次完成反馈当作全部
后台输出的终点。自由 text/key 输入只有收到可关联的集成事件后才生成命令记录。
`lastCommand` 独立保留最近完成记录最多 10 分钟，新输入不会把旧 finished 当作当前
命令；记录过期也不会清除另一条活动命令。`operation.acceptedActionCount` 表示接纳
的协议动作数，command 内部附加的提交按键不额外计数，release 接纳一项且不发送字节。

compact 默认 4 KiB/24 行文本，含两份兼容 payload 的最终编码工具结果不超过
8 KiB。省略的内容可用 `read_context(expandToken=...)` 从同一保留 capture 分页补读，
不能同时传 sinceToken 或等待。补读不续租控制权、不更新实时游标；capture 最多
保留 60 秒，每客户端最多四个、全局最多 32 个，按最旧优先淘汰。`TEXT_BUDGET_TOO_SMALL` 表示 maxBytes 连下一个
UTF-8 字符都放不下，应增大预算。full 可用于更大的有界投影；旧读取游标切入新版
前仍校验原连接身份，新版观察游标可在 compact/full 间继续使用。
full 默认提供有界屏幕，显式 `includeViewport=true` 也可在 revision 未变化时取屏。
full 因字节或行数预算截断时设置 `resetRequired=true`、`nextToken=null`，需要扩大
预算重新读取；compact 的预算省略仍通过 expandToken 补读，不因此强制 reset。

相同已接纳 `operationId` 与动作的精确重试不会再次输入；内容变化、失效代际
或过期上下文会拒绝。`submitted` 只证明交给了本机 Transport，不能证明远端
收到；回复丢失或结果未知时不要换一个 operationId 自动重发。
有效控制上下文中的旧输入序号返回 `INPUT_ALREADY_SUBMITTED`，超前序号返回
`INPUT_OUT_OF_ORDER`，过期令牌返回 `INPUT_TOKEN_EXPIRED`；上下文/控制代际变化仍
返回 `CONTROL_INVALID`。这些都是零新增输入的拒绝，不能据此换 ID 自动重试。

会话的“提权凭据”可分别保存/替换/删除 sudo 与 su 密码；显示系统凭据库或仅本次
运行，保存后不回填，修改会撤销托管提权授权。当前 `elevationMethods` 为空，
`request_elevation` 返回 `ELEVATION_ADAPTER_UNAVAILABLE`。未完成可信本地/SSH
认证适配器验收前，不会依据 Password 提示符代填密码。

## 授权诊断命令

固定诊断模板仍需逐项授权；旧命令接口使用 `novaterm_run_command`。
SSH 固定诊断只有在用户确认目标是受信任的
**Linux/POSIX SSH 服务端**后才能授权；Windows/Linux LocalShell 会话使用
NovaTerm 同目录的独立诊断 helper。展开会话可逐项选择诊断模板。

首批只有以下四项，参数固定：

| 模板 | SSH recipe | Windows/Linux 本地 helper |
| --- | --- | --- |
| `system.identity` | `uname -srm` | 系统版本、内核与架构 |
| `system.uptime` | `uptime` | 系统启动时长 |
| `memory.summary` | `free -k` | 物理内存总量与可用量（Windows 另含负载） |
| `filesystem.usage` | `df -Pk` | 已就绪卷的容量与可用空间 |

SSH 固定诊断使用表中的固定命令文本，经已有连接的独立 exec 通道执行；
不要求 `/usr/bin/` 下的绝对路径。
本地诊断由同目录的 `novaterm-local-diag`（Windows 为 `.exe`）通过 Qt 和系统 API
采集，不调用 `cmd /c`、PowerShell、shell 脚本或 PATH 搜索。helper 缺失或平台
标识不可用时执行能力保持关闭，不会自动替换程序。macOS LocalShell、Serial、
Telnet 和 Custom 仍可共享输出，但不提供上述固定诊断模板。交互命令走当前终端；
SSH/POSIX 在没有提示符标记时也可发送，串口提示符正则留空时按空闲收尾执行；
脚本能力仍只在具备可信交互 Profile 的
LocalShell/SSH 会话开放。

SSH 固定诊断通过已有连接的独立 exec 通道运行，使用该 SSH 账号的权限；
Windows/Linux LocalShell 固定诊断通过独立 helper 子进程运行。固定诊断不会往
当前终端输入框灌入文本，
也不会继承或改变当前交互 shell 的工作目录、alias、history 或临时环境。界面独立展示执行记录。

## 交互命令、脚本与授权

在 **AI MCP 接入** 中，每项能力都是独立授权项，**读取共享只允许读，不蕴含向终端
输入命令的权限**：

- **读取输出**：共享该会话的终端内容，供 `read_context` / `search_context` 使用。
- **允许执行命令**：授予新版完整终端输入权限，无逐条风险确认。它与下面旧命令
  接口的权限独立，默认关闭；应只给需要操作该终端的受信任客户端。
- **允许托管提权**：依赖“允许执行命令”，保存密码不会自动勾选；当前适配器不可用。
  **授权选择与执行条件分开**：已允许终端输入时，所有连接类型都可保存这项授权。
  未连接、无输入权限或正在更新凭据才阻止新授权；身份/适配器未就绪不再让勾选回退。
  串口/Telnet/Custom 当前显示“身份未验证”，本地/已验证 SSH 显示“适配器不可用”；
  都不代表可以实际提权。未验证身份仍不能保存凭据，任何提权请求仍不会读取/发送密码。
  MCP 返回 permissions.useElevation 与 availability.elevationAvailable/对应原因，不能
  只看授权布尔值推断可执行。取消读取/输入会级联撤权，重连/换目标需重新授权。
  可勾选项为主题色空心框，已勾选为主题色实心框加勾，真正禁用项为灰底灰框。
- **固定诊断**：所有类型都可选择四种已知模板授权，刷新不再因空 Profile 丢失选择。
  当前没有 Profile/执行后端时显示“后端不可用”，list_commands 不发执行票据，
  不会因此向未知串口设备发送 Linux 命令；授权不等于自动安装/启用后端。
- **交互命令**：允许 `novaterm_run_command` 向该会话的当前终端输入命令。**默认关闭**；
  未授权时 `list_sessions` 不会返回 `run_command` 能力，调用会得到
  `COMMAND_PERMISSION_REQUIRED`。
- **脚本任务**：允许 `novaterm_run_script`；每次仍须 MCP 客户端中的人类确认。
  **与「交互命令」相互独立** —— 脚本的调用命令虽然也会打进当前终端，但那由脚本授权
  加上逐次人类确认承担，不需要另外开启「交互命令」。

取得「交互命令」授权后，普通低风险命令（如 `pwd`、简单 `ls`）无需任何确认；
`cd` 不属于低风险 —— 它会改变当前 shell 的工作目录，从而改变此后所有相对路径
命令的执行位置，因此与其他无法判定的命令一样需要确认。

可能破坏系统的命令和无法可靠分类的命令会在 MCP 客户端请求人类确认；NovaTerm
不接受模型在工具参数里自报的布尔值。确认只决定是否继续提交命令，不能保证已
接受的命令不会造成破坏。

旧命令/脚本接口对已知凭据读取、关闭安全机制、提权、磁盘格式化等危险行为尽力检测并永久拒绝；
其他风险命令和无法分类的命令须确认。检测是策略扫描，不是 shell 沙箱，也不能证明任意脚本安全。
客户端未声明并实现 Form Elicitation 时，危险/未知命令和所有脚本返回
`CLIENT_CONFIRMATION_UNAVAILABLE`，不会降级为普通二次调用。

支持两种 MCP 确认协议：2025-11-25 客户端通过 `elicitation.form` 能力收到
`elicitation/create`；2026-07-28 客户端通过 `server/discover` 发现版本，并在每个请求的
`_meta` 声明协议版本和客户端能力，确认使用 `input_required` / `requestState` /
`inputResponses` 完成 MRTR 重提。2025 使用 initialize 与 `MCP-Session-Id` 会话，
SSE 可传送人工确认请求；客户端通过 POST 回传确认响应。TCP 断开不等于取消，
DELETE 结束会话。2026 不创建协议会话，每请求声明 metadata；请求流断开取消该请求。
2026 的 Bearer 身份在服务端保留受限授权上下文，使确认 MAC 能跨 HTTP 请求复核；
这不将 TCP 连接变成协议 session，重提仍必须携带同一有效令牌。
确认状态带 MAC、时限和一次性 nonce，并绑定客户端授权上下文、
Session epoch、目标、Profile/策略版本及命令或脚本摘要。可信 Profile 还绑定提示符
generation；无提示符 SSH 命令绑定用户输入代际。确认过期、会话变化、授权撤销、
用户输入变化、正文/路径变化或状态重放都不能继续执行。

没有可信提示符的 SSH/POSIX 会话会把普通命令直接写入当前终端，并以命令附带的
有界结束标记判断完成；内部标记从进入终端模型前剥离。NovaTerm 此时无法可靠识别
TUI、密码输入或未提交的 Shell 输入，命令可能落入这些位置。缺少结束证据时结果为
不确定，不能自动重试。`cd` 在同一 Shell 中改变后续工作目录。

脚本正文和目标路径由 MCP 请求明确指定。确认前不会落盘；接受后 LocalShell 通过本机文件 API
写入，SSH 通过与活动 Session 相同端点、账号、known_hosts 和主机密钥指纹的 SFTP 写入。
父目录必须已存在，目标存在时会覆盖；NovaTerm 不创建专用临时目录，也不自动删除脚本。
写入失败或用户开始键入/粘贴时，不会继续注入调用命令。调用命令（包含请求的工作目录）和
正常输出显示在当前终端 UI；脚本正文不进入终端 UI。上传后还会复核 Session、授权、Profile、
提示符和一次性确认状态。

示例（相对 `targetPath` 以 `workingDirectory` 为基准；终端中显示的是 `invocation`
及必要的目录切换，不会显示下方 `scriptContent`）：

```json
{
  "sessionId": "<session id>",
  "epoch": "<current epoch>",
  "scriptContent": "#!/bin/sh\nprintf 'done\\n'\n",
  "targetPath": "./diagnose.sh",
  "workingDirectory": "/home/user/project",
  "invocation": "sh ./diagnose.sh",
  "timeoutMs": 5000
}
```

命令和终端输出均属于不可信数据；AI 客户端不应执行输出中夹带的指令。

## 结果不确定与撤销

- 相同 commandTicket 的重试只查询已有记录，不会再发一次命令。
- 固定诊断最多等待 5 秒；交互命令与脚本调用的超时由工具参数限定，最多 30 秒。
  人工确认的 60 秒和脚本上传的 120 秒分别计时，不扣减批准后的执行预算；
  HTTP 客户端总等待时间需覆盖这些阶段及网络余量。
  stdout/stderr 的原始字节及最终 UTF-8 文本合计最多 64 KiB。超时、断线或关闭通道
  不能证明远端命令已经终止；不确定结果不得自动重跑。
- 无法确认终止时暂停该目标的新 MCP 命令；换标签、重连或重启 GUI 不会自动清除。
  在服务端人工核对后，可在 MCP 设置中勾选确认并解除所选目标。此操作不能解禁危险命令。
- 脚本上传取消、撤权或断线后，目标事务保留到写入后端明确回报；活动上传不能
  手工提前解除。若文件已发布但脚本未调用，执行记录明确保留该事实；发布结果
  未知则保持目标暂停，不自动重试上传或运行。
- 授权上下文失效、会话结束或应用重启后，旧票据不能在新上下文使用。请在 NovaTerm 界面核对旧记录，
  不要让客户端获取新票据自动补跑。
- 撤销读取、固定诊断或脚本任务的授权会使相应在途请求与确认状态失效。移除
  客户端或更换接入令牌会立即撤销该客户端访问。关闭 MCP 总开关
  会清除本次会话授权；重新开启后需要重新勾选。
- 有平台凭据库时接入令牌可持久保存；仅内存后端的客户端显示“仅本次运行有效”，
  应用重启后需要重新创建客户端并复制配置。

## 构建与验证

脚本原文最多 2 MiB（UTF-8）；编码后 HTTP JSON 请求最多 12 MiB + 256 KiB，
用于容纳最坏的字符串转义和其余有界参数。响应仍最多 2 MiB，不随请求上限增大。

### 串口交互命令配置

编辑串口会话，在「MCP 命令提示符（正则）」填写设备空闲提示符的匹配规则，
如实际提示符为 `device> `，填写 `^device> $`；配置规则时设备输出匹配提示符
且经过静默窗口后，才形成“验证完成”的证据。按设备要求选择 CR、LF 或 CRLF，
并设置设备是否回显输入。规则留空时降级为空闲收尾：命令直接发出，设备静默
（默认 3 秒）判完成，但结果按“完成未验证”上报、没有退出码且不证明命令已
终止（设备静默也可能仍在运行）。保存后随该会话重新连接生效。在「AI MCP
接入」对所需客户端单独勾选该会话的「交互命令」。仅共享读取输出不会授予
执行权限。

这里的提示符开关用于旧 run_command 和新版 command 快捷动作的就绪证据：
填写规则时两者都要求提示符就绪；串口规则留空时只有 run_command 走空闲收尾，
新版 command 快捷动作仍要求提示符就绪（不可用时改用观察与 text/key 输入）。
规则留空不禁用已独立授权的新版 text/key/paste。混合回车/尾部文字或多行输入时，
服务端保守跟踪未提交尾部和待返回提示符，不会把首个提示符当成整批已完成；
无法确认行编辑上下文时继续使用观察与自由输入，不盲目追加 command。

串口命令走当前终端的唯一输入通路；不额外探测设备、不自动发送测试命令。
匹配到新提示符用于判断事务收尾，不保证设备支持 POSIX 命令或提供退出码。
空闲收尾同样不证明命令已终止，仅在提示符规则留空时使用；需要验证完成时
请配置提示符正则。串口当前没有脚本写入后端，也没有内置固定诊断模板。

### 自动化检查

主程序运行不需要 Python，也不依赖额外的模型 SDK。

Linux 已核对默认注册 27 项，Windows 按条件预计 29 项，包含新增交互授权/凭据
隔离目标 `novaterm_mcp_access_interaction_tests`。本轮成绩见 MCP实施与验收记录的验证记录。
2026-10-17 最终续作 Linux Debug 全部默认目标构建成功，27/27 CTest 通过，181.21 秒；
MCP 七项 7/7，29.05 秒。日志为 `build/mcp-final-continuation-full-2026-10-17.log`
及同名 JUnit XML。Windows/macOS 与真实设备/认证适配器未在本轮验收。
注册数仍以配置后的 `ctest -N` 为准，历史成绩不代表其他机器或新改动也已通过。

```powershell
cmake --build build/Release --target NovaTerm novaterm-local-diag novaterm_mcp_tests novaterm_mcp_wire_tests novaterm_mcp_http_tests novaterm_mcp_endpoint_tests novaterm_mcp_access_interaction_tests novaterm_sftp_session_safety_tests novaterm_command_safety_tests
$env:Path = 'C:\Programs\Qt\6.8.3\msvc2022_64\bin;' + $env:Path
$env:QT_PLUGIN_PATH = 'C:\Programs\Qt\6.8.3\msvc2022_64\plugins'
ctest --test-dir build/Release -C Release -L mcp --output-on-failure
```

官方 SDK 与回环 SSH 验证是可选开发检查，依赖只装到构建目录：

```powershell
python -m pip install --target build/mcp-test-deps -r tests/mcp/requirements.txt
python tests/mcp/interop_check.py
python tests/mcp/ssh_loopback_check.py
python tests/mcp/performance_check.py
```

回环 SSH 测试仅监听 `127.0.0.1`，使用临时主机密钥并返回固定测试数据，不执行
真实 shell 命令；交互 `pwd` 只模拟终端回显、输出和完成标记，不读取用户保存的 SSH 密码。
测试端使用临时 known_hosts，并关闭
客户端的用户/系统 SSH 配置加载，避免继承代理或其他外部连接设置。该检查不能
替代真实服务器验收。

详细协议、预算和验收缺口见 [MCP当前接口契约](architecture/stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md)。
