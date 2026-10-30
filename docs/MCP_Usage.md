# NovaTerm AI MCP 使用说明

2026-10-30：当前二十工具。所有 AI 共用一套接入令牌和授权；首次启用自动建立统一接入。会话目录只列当前已连接且获准读取的会话；每个新就绪连接默认启用读取、完整输入、托管提权和清空四项权限。同一连接内手动撤权不会被刷新恢复。保存目标目录/打开/查询/关闭四工具和对应界面已移除，手动保存会话入口保留。当前改动的验证结果以本轮记录为准，不沿用历史测试成绩。Linux安全读取源已实现，Linux安全写入及macOS文件后端仍未实现；托管提权适配器仍不可用。参数见[现行契约](architecture/stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md)，历史成绩见[验收档案](architecture/stages/P8_AI_MCP_Interface/records/README.md)。

NovaTerm 提供进程内 Streamable HTTP MCP 服务，可让支持 MCP 的 AI 客户端读取指定终端输出、
搜索已读取的内容，并按统一授权操作当前交互终端。MCP 总开关默认关闭。

历史录制仍在主菜单 **历史录制** 中手动开始/停止。启用统一 MCP 后，已有录制可直接列出、读取、搜索，无须选择录制条目再逐条授权；关闭终端不删除历史。历史工具不自动启动录制或恢复设备连接。独立日志导入用于创建本次运行的分析资源，最多 16 MiB；可清除已导入资源。

## 直接路径传输与一次确认

不再有独立的文件授权窗口、共享目录选择或串口发送目标授权。提示词给出文件路径和已连接目标即可，例如“把 `D:\build\firmware.bin` 上传到开发板 `/home/root/firmware.bin`”。本地路径属于 **运行 NovaTerm 的机器**。

首次调用上传工具返回 `state=confirmation_required`，包含源路径、目标会话/路径、覆盖选项及 `confirmationToken`。外部 Agent 把这些信息展示给用户，**只询问一次是否上传**。用户接受后，Agent 自动以相同参数和 operationId，附 `confirmationToken`、`confirmed=true` 续调；拒绝用 `confirmed=false`，不会发送文件。NovaTerm 不再弹第二个确认窗口。

相同源路径、目标端点/位置、协议及覆盖选项会在本机记忆，后续新操作直接执行，无须再次询问。记忆按路径，不是按文件内容 hash；同一路径重新构建后的文件沿用确认。路径/目标/覆盖变化重新确认。设置页的 **清除上传确认记忆** 会清除记忆和待确认请求；令牌更换后的新主体不继承旧令牌记忆。确认凭据有效 5 分钟，只能用于原身份、原连接和原参数，待确认最多64项，记忆最多512项。

SFTP 自动通过原会话的后台 owner 准备就绪，不必先打开文件面板；人工文件作业在进行时等待原后端空闲。串口发送自动衔接本 AI 的输入控制，接收端仍需先准备好。不会自动运行上传文件。

Windows/Linux支持安全本地读取，Linux Writer和macOS后端仍未实现。保持64 MiB文件、64 KiB分块、固定句柄与类型检查；Linux flock为协作锁，源目录其他写入者须可信。

## 开始使用

1. 编译并运行 NovaTerm，Qt SDK 需要包含匹配版本的 **HttpServer** 组件。
   MCP 服务由主程序持有，不需要独立 MCP 桥接进程。
2. 在主菜单打开 **AI MCP 接入**，勾选 **启用本机 MCP 接入**。
3. 首次启用自动创建统一令牌，不再添加或选择客户端。所有 AI 使用同一份配置；**重置接入令牌** 后需为所有 AI 更新配置。旧多客户端配置不会自动合并或继续认证。
4. MCP 会话目录只显示已连接会话。每个新就绪连接默认勾选 **读取输出**、**允许执行命令**、**允许托管提权** 和 **允许清空终端**；可逐项取消。同一连接内刷新不会恢复手动撤销，重连形成新绑定后重新应用默认选择。完整输入允许文本、按键、粘贴和命令，不逐条确认；托管提权仍受可信目标及适配器能力限制。
5. 点击 **复制 MCP 配置**，粘贴到所使用的 MCP 客户端配置中。配置含接入令牌，
   不要公开、提交到仓库或发送给其他人。
6. 使用 CC Switch 时点击 **复制 CC Switch 配置**，把单个 server 对象粘贴到
   「完整的 JSON 配置」编辑器，并将唯一标题设为 `novaterm`。不要额外包裹
   `mcpServers` 或 `novaterm`；CC Switch 会用标题作为服务器 ID。

只有当前已连接且获准读取的会话可见。MCP不自动启动NovaTerm或打开保存连接；请先使用手动会话入口建立连接。切换当前标签不会改变工具的目标，调用按 sessionId 和 epoch 定位。

默认接入地址为 `http://127.0.0.1:38973/mcp`，应用重启后保持稳定；应用启动前
可用环境变量 `NOVATERM_MCP_HTTP_PORT` 覆盖端口。端口占用会明确报错，不自动
换端口。需要多个应用实例时为各实例显式选择不同端口，使用各自导出的 URL 和令牌。
应用必须运行且已启用 MCP，客户端不会自动启动主程序。应用重启后的新就绪连接同样应用默认会话权限；历史自动可读；已记忆的上传路径按原确认组合复用。

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

## 五个现行终端工具

| 工具 | 用途 |
| --- | --- |
| `novaterm_list_sessions` | 列出当前已连接且统一接入获准读取的会话及身份 |
| `novaterm_read_context` | 缺省读取屏幕/摘要；source=log 按精确位置读取保留的终端历史 |
| `novaterm_search_context` | 搜索一次已返回的 capture，不搜索整个历史或执行正则 |
| `novaterm_interact_terminal` | 按观察返回的输入令牌操作当前终端，并等待下一次有用反馈 |
| `novaterm_request_elevation` | 请求托管 sudo/su；当前没有可信认证适配器，返回不可用且不读取密码、不输入命令 |

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

当前软尾行、未换行提示、CR 进度条和备用屏仍看缺省 observation。日志只读已稳定的终端缓存，没有原始字节或覆写前屏幕帧；分页完整不证明 UART 硬件零丢失。日志 token 与观察 token/capture/expandToken 不混用，不产生 inputToken；需要操作时另读屏幕。MCP 清空按下节独立授权开放；文本等待和长任务见下文；文件传输当前提供受限SFTP上传/下载和独立授权的UART/ZMODEM单文件发送。

## 文本等待与长任务（B3/B3a）

当前共二十个工具：五个基础终端工具、三项历史、清空、四项等待/任务、SFTP上传/下载/查询/取消四项，以及UART上传/查询/取消三项。AI从已连接会话目录取得sessionId/epoch，用户不用手工填写内部ID。

`novaterm_list_file_resources`、`novaterm_list_serial_file_targets` 已退役，旧 resourceId/scopeId/targetId 与 revision 参数明确拒绝。保存目标和旧命令/脚本工具仍退役。

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

单次等待最长30秒、默认扫描256KiB/最大1MiB。timeout或scan_budget_exhausted返回resumeToken，重传同一匹配条件继续（不再带sinceToken）；可以调整waitMs/scanBytes。续扫状态固定十分钟，统一接入8/全局32，过期明确拒绝。nextToken是服务端扫描进度；需要完整日志另从startToken分页，不把命中上下文当作已读全文。gap/invalidated须承认缺口并显式选新基线；pending/busy冷页等待不证明没有文本。取消等待不发送任何输入。

长任务启动示例：

```json
{"sessionId":"<id>","epoch":"<epoch>","inputToken":"<fresh observation>","operationId":"<new UUID>","actions":[{"type":"command","command":"make test"}],"timeoutMs":600000}
```

启动只做一次；短RPC断流不停止已受理任务。用taskId查询，若启动响应丢失，用原operationId查询受理；网络重试使用相同ID和全部相同参数，不换ID重输。任务统一接入32/全局128、终态保留十分钟；应用重启或关闭MCP不恢复执行索引，记录丢失时报告未知，不能自动重跑。

分别检查status、submissionStatus、completed、evidenceKind、exitCode和terminationConfirmed。UART原始任务静默仍running，执行期限到达unknown，不能捏造exitCode=0；SSH提示符也不证明功能测试成功。停止请求与远端确认终止分别解释；人工接管、可信环境reset、断连或新执行后旧停止请求不再有效。任务输出仍来自终端缓存，清空/淘汰可造成output缺口，但不抹除执行事实。

构建、部署、固件标识和功能判据由外部Agent分别确认；文本命中只证明文本出现。完整字段、预算和验收边界见 [现行契约](architecture/stages/P8_AI_MCP_Interface/02_Current_MCP_Contract.md)。

## SFTP 上传与下载

`novaterm_upload_file` 和 `novaterm_download_file` 接受 sessionId、epoch、localPath、remotePath、operationId；overwrite可选，默认false。路径必须是完整文件路径，父目录须存在。上传确认流程见上节，下载按明确工具调用直接执行。

```json
{"sessionId":"<id>","epoch":"<epoch>","localPath":"D:/build/firmware.bin","remotePath":"/home/root/firmware.bin","operationId":"<new UUID>"}
```

上传返回待确认后，接受时在原参数中增加：

```json
{"confirmationToken":"<returned token>","confirmed":true}
```

用 transferId 调用 `novaterm_query_transfer` 查询，`novaterm_cancel_transfer` 取消。相同operationId只能精确重试，改变方向/路径/覆盖意图会冲突；确认重试不会重复发送。未知发布结果保留原ID查询，不自动换ID重传。取消不删除已发布文件，下载不删除远端源；contentVerified仍为false。

安全本地Writer目前仅Windows实现；上传和下载复用原SFTP队列/worker。新建远端目标要求hardlink扩展1，覆盖已有目标要求posix-rename扩展1。仍需可信目录属主/管理员，SFTPv3不能抵抗同账号恶意换路径。同步库调用不保证硬截止时间。

## SFTP显式覆盖

`novaterm_upload_file`和`novaterm_download_file`接受可选 `"overwrite": true`；省略或false都保持默认不覆盖。同operationId改变该值会冲突，其他独立读写权限不变。

true明确授权可信目录中的目标路径替换，不是按文件身份执行的条件替换。程序先记录原目标、发布前再次检查，发现替换、删除、类型或属性变化会拒绝，但最后检查与rename之间仍有竞争窗口。只在其他写入者可信的目录使用；本地目录句柄固定并没有检查ACL/owner，不能以此认定写入者已经过安全审查。

即使true，初始明确不存在的目标也继续走不覆盖发布，后来的竞争创建会拒绝；查询失败、拒绝访问或共享冲突不当作不存在。已有目标须是安全普通文件；本地readonly、外部句柄阻止替换时会失败，工具不清目标只读属性、不强关句柄、不先删再改名。远端已有目标要求posix-rename扩展版本1，新目标仍要求hardlink版本1；不支持时明确拒绝。

替换使用新临时文件的属性，不自动复制旧ACL、属性或执行位，远端新文件仍0600。发布前取消/撤权只清理本次临时文件，发布后的新目标保留。远端失去发布响应返回unknown，临时文件消失不证明目标已经替换；保留原ID查询，不自动换ID重试。下载发布事实仍只指本地，contentVerified=false。当前覆盖阶段Windows软件验证已完成，见[覆盖记录](architecture/stages/P8_AI_MCP_Interface/records/B3b_File_Overwrite_2026-10-28.md)。

## UART/ZMODEM单文件发送

1. 选择已连接串口，准备设备接收端（例如适当设备上的rz）。
2. 调用 `novaterm_upload_serial_file`，提供sessionId/epoch、localPath、operationId，remoteName可选，省略取源文件名。首次上传与SSH同样只询问用户一次，Agent自动带确认凭据续调；记忆命中直接发送。
3. 本主体输入控制自动释放以交接原ZMODEM控制器，无须额外release调用。尚未提交完的输入、其他主体控制和现有文件传输仍报告忙碌，不抢占。
4. 通过 `novaterm_query_serial_transfer/cancel_serial_transfer` 使用原transferId查询或取消。

```json
{"sessionId":"<serial id>","epoch":"<epoch>","localPath":"D:/build/firmware.bin","operationId":"<new UUID>"}
```

接收目录及覆盖行为由设备接收端决定。不会自动rz或执行文件。completed需要protocolCompleted且fileConfirmed；ZSKIP表示skipped，不是已送达。remotePublication仍unknown、contentVerified仍false，hash/落盘须另行核实。

## 本地清空（B2）

新就绪连接默认允许 MCP 清空；在本机 MCP 设置中可取消该会话的清空权限。权限有效时，先读取一次日志页，再显式调用 `novaterm_clear_terminal`：

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
保留 60 秒，统一接入最多四个、全局最多 32 个，按最旧优先淘汰。`TEXT_BUDGET_TOO_SMALL` 表示 maxBytes 连下一个
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

会话的“提权凭据”可分别保存/替换/删除 sudo 与 su 密码；存储方式尚未知时明确显示
“尚未确认”，实际后端确认后显示系统凭据库或仅本次运行。保存后不回填，修改会撤销
托管提权授权。当前 `elevationMethods` 为空，
`request_elevation` 返回 `ELEVATION_ADAPTER_UNAVAILABLE`。未完成可信本地/SSH
认证适配器验收前，不会依据 Password 提示符代填密码。

## 现行授权与旧接口退役

主界面保留统一接入开关、配置复制、令牌重置、已连接会话权限、接管和清除上传确认记忆，不再有客户端管理或保存目标表格。

- **读取输出**：允许读取/搜索当前会话，；已有录制由统一MCP接入直接读取。
- **允许执行命令**：允许所有持有统一令牌的 AI 输入文本、按键、粘贴和命令，不逐条确认。
- **允许托管提权**：依赖完整输入；当前可信认证适配器不可用，勾选不代表能够执行提权。
- **允许清空终端**：只影响本机显示/回滚，不停止设备程序、不删除独立历史录制。

四项在每个新就绪连接默认开启。同一连接内的手动撤权持续有效；取消读取级联撤销输入、提权及清空，取消输入撤销提权。文件与UART直接指定路径，首次上传按工具返回提示确认一次；历史无需逐条共享。

`novaterm_list_commands`、`novaterm_execute_command`、`novaterm_run_command`、
`novaterm_run_script` 已从工具目录、解码及执行接线中删除，直接调用返回未知工具。
客户端应刷新工具目录；旧接口不会转发到权限更宽的新终端输入。旧多客户端授权不会合并；新统一接入按上述连接默认规则建立权限。原来的固定诊断/脚本/交互授权列、旧执行结果面板、
“确认远端停止”和“解除暂停”入口已移除。

诊断或命令通过 `novaterm_read_context` 观察，再以 `novaterm_interact_terminal`
操作已授权终端；长任务使用 `novaterm_start_task/query_task/stop_task`。
文件先通过直接路径传输工具发布，再由已授权终端显式执行，不恢复旧脚本发布入口。

## 结果不确定与撤销

输入受理、任务停止请求、HTTP断开或释放终端控制，都不能单独证明远端进程结束。
不确定结果按原operationId查询/精确重试，不用新ID重复提交。人工接管后不得停止新命令。
撤销读取会级联撤销输入、提权及清空；令牌重置和关闭总开关撤销相应访问。
已发给设备的输入不能撤回，必要时由用户在终端或设备侧确认实际状态。

## 构建与验证

脚本原文最多 2 MiB（UTF-8）；编码后 HTTP JSON 请求最多 12 MiB + 256 KiB，
用于容纳最坏的字符串转义和其余有界参数。响应仍最多 2 MiB，不随请求上限增大。

### 串口交互命令配置

编辑串口会话，在「MCP 命令提示符（正则）」填写设备空闲提示符的匹配规则，
如实际提示符为 `device> `，填写 `^device> $`；配置规则时设备输出匹配提示符
且经过静默窗口后，才形成“验证完成”的证据。按设备要求选择 CR、LF 或 CRLF，
并设置设备是否回显输入。配置随该会话重新连接生效。当前 `interact_terminal` 的 command 快捷动作要求提示符就绪；规则留空或证据不足时使用观察与 text/key 输入。旧 `run_command` 已移除，不再提供其空闲收尾入口。
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
