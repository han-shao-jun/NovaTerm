# 指定文本等待、长任务、文件传输与目标连接

更新：2026-10-31。当前二十工具；统一令牌和授权，MCP仅列出已连接会话。文件使用直接路径，首次上传确认并可记忆；下载直接执行。Windows和Linux安全本地文件读取与写入后端已实现，macOS后端未实现。UART接收端由调用方准备，同主体空闲输入控制可自动衔接。B4尚未整体完成。

现行字段见[02](02_Current_MCP_Contract.md)，阶段进度见[覆盖记录](records/B3b_File_Overwrite_2026-10-28.md)，历史子批见[验收档案](records/README.md)。

## 1. 最小交付范围

| 差距 | 补齐方案 | 边界 |
| --- | --- | --- |
| 缓存不易回查 | 屏幕、当前尾部书签、保留起点和精确位置续读 | 复用终端缓存，不新建日志数据库 |
| 等待指定结果需反复往返 | 有界字面文本等待，明确日志或屏幕来源 | 不迁入脚本引擎或开放任意正则 |
| 长任务缺少直接入口 | 异步启动、查询、请求停止 | 复用已有执行 owner，不复制命令执行器 |
| 二进制部署文件交付 | 直接路径的SFTP上传/下载与UART/ZMODEM发送、查询和取消；显式覆盖已完成Windows软件验证 | 不自动准备接收端、烧录、重启或执行文件 |
| 连接会话 | 用户在本机建立连接，Agent列出并使用已连接会话 | 不提供保存目标管理工具 |
| 使用说明冲突 | 两代握手、工具描述与使用文档的统一规则 | 未实现能力不得出现在可调用工具指引中 |

日志/等待/长任务与文件/连接保持独立能力。下文描述职责和边界；可调用工具名、精确参数与预算以02和源码为准，显式覆盖已完成Windows软件验证，不能据源码入口宣称B4已验收。

## 2. 指定文本等待

输入包含明确的 Session/epoch、读取来源、条件文本、起点和等待期限。它是只读用例，不发送探测字符，不获得输入权。

| 来源 | 起点 | 匹配范围 |
| --- | --- | --- |
| 日志 | 调用方日志游标；未提供时以明确返回的当前尾部建立基线 | 只扫描该位置之后仍保留的文本，不能用上轮 PASS 立即满足本轮等待 |
| 当前屏幕 | 显式选择当前快照或基线之后变化的快照 | 用于未换行提示、U-Boot、TUI；返回屏幕 revision 与捕获时间，不伪装日志位置 |

首版支持字面匹配、明确大小写选项和有界的候选列表；any/all 的精确枚举在 B0 冻结。all 只表示范围内均出现过，不证明顺序。首版不开放任意正则，复杂分析留给外部 Agent。

结果区分 matched、timeout、cancelled、gap、invalidated 和扫描预算耗尽；返回命中的条件、位置/屏幕版本、有界上下文和下一观察位置。匹配到 PASS 只证明该文本出现，不自动把任务或功能测试判为成功。

日志匹配跨页、跨 chunk 和 UTF-8 分段保留有界尾部；不删除换行后拼接无关文本。匹配后的进度按明确消费边界推进，不跳过未返回的后续日志。扫描预算耗尽有可续扫的位置和匹配状态，不从断点任意重置匹配器；状态有 TTL 和总量上限，失效明确拒绝。

等待前、唤醒时、交付前均复核授权和绑定。清空、缓存淘汰、重连或撤权返回明确失效/缺口，不能静默换起点。屏幕匹配不跨屏幕版本拼接文字。超时返回有效进度，但不代表错误不存在。

单次等待不超过现有 30 秒上限；长等待由调用方续接。候选数、条件长度、扫描字节、每轮 CPU 工作量和每客户端/全局并发都必须有上限。取消等待不发送 Ctrl+C，不停止远端命令。

## 3. 长任务入口

| 操作 | 输入与行为 | 输出事实 |
| --- | --- | --- |
| 启动 | 目标、最新输入条件、operationId、命令/动作、执行模式及期限，经既有授权/仲裁提交 | taskId、关联 executionId（若有）、受理/提交状态、起始日志位置 |
| 查询 | taskId、可选日志游标；不再次执行 | 状态、完成证据、退出码（可空）、有界新增输出/续读位置、建议查询间隔 |
| 请求停止 | taskId、幂等操作 ID、明确停止意图；核对当前执行与控制代际 | 已请求停止、已确认终止或结果未知，不把返回成功等同于远端已停 |

taskId 绑定可信客户端与目标，不等于 RPC ID、日志游标或 PID。启动精确重试返回同一任务；同 ID 不同参数拒绝。查询不得延长输入权限或自动抢回人工控制，停止不得影响别的客户端或新执行。

CommandRuntime/Session Controller 继续拥有执行事实；新增异步任务索引只保存关联和有界结果，不复制超时、取消、输入、完成解析状态机。输出引用现有缓存，清空/淘汰后任务仍可返回执行事实和日志缺口；不能为任务无限保留旧块。

状态至少区分受理、运行、停止请求和终态；completed、exitCode、terminationConfirmed、evidenceKind 分别解释。UART 静默等待结束没有可靠完成证据时保留未验证/未知，不能合成为 exitCode=0。超出执行期限按现有取消与未知保护收口。

### 传输差异

- SSH 优先操作现有可见终端，保留 cwd/env。独立 exec 若另行支持，必须显式声明执行模式，不能静默改成隔离子 Shell。
- UART 支持获准原始输入后的异步观察，不要求 POSIX 包装；是否可验证完成、是否可请求中断独立报告。不能照搬 Netcatty 拒绝 Serial 后台任务的限制，也不能假装 Serial 有可靠退出码。
- 发现结果分别表达可启动、可观察、完成证据类型、可停止及不可用原因，而不是单个 supported=true。
- Ctrl+C 只在原任务/目标/控制代际仍匹配且明确获准时发送。人工接管后，旧停止请求不得打断人的新命令；无可靠停止能力时如实报告。

新异步启动被应用受理后，短 RPC 断流不删除任务或重复提交；不改写旧同步工具的取消语义。调用方可通过原 operationId 查询受理结果。首版不跨应用重启恢复执行，失去记录时报告不可用/未知，不自动重跑。活动任务、保留结果、日志引用与并发均有硬预算。

## 4. SFTP与UART/ZMODEM单文件交付

使用统一MCP接入和已连接会话；SSH上传/下载传入完整localPath和remotePath，UART上传传入localPath和可选remoteName。首次上传先取得确认并由用户确认一次，相同组合可记忆。旧文件授权目录和相关resourceId、scopeId、targetId参数已退役。

### SFTP上传

`novaterm_upload_file/query_transfer/cancel_transfer`复用终端面板原SftpSession、队列与worker，使用直接路径与上传确认。当前仅普通文件、默认不覆盖；安全源在worker内使用固定句柄，远端使用随机临时文件与hardlink扩展原子创建新目标。取消只清理本次拥有的临时文件，保留已开始发布的成功/未知事实。具体路径安全、SFTPv3限制与预算见[契约](02_Current_MCP_Contract.md)和[上传记录](records/B3b_Sftp_Upload_2026-10-27.md)。

### SFTP下载

`novaterm_download_file`复用原 `novaterm_query_transfer/cancel_transfer`，FileTransfers统一记录方向和幂等事实，同一SftpPanel/SftpSession队列/worker执行，无新连接或线程。下载子批只新增downloading；本覆盖阶段仅为upload/download两输入追加可选boolean overwrite及说明，其他Schema和旧fixture保持。

必填sessionId、epoch、localPath、remotePath和operationId；overwrite可选且默认false。身份和原会话绑定持续复核；同operationId跨方向冲突，精确重试/查询不续期或重传。

原worker检查远端根/叶，打开一次源句柄并按观察大小读取，补EOF探测和读取前后属性复核；分段短读可累计，提前EOF、增长、读错或不安全属性不能发布部分文件。SFTPv3仍要求授权目录属主/管理员可信，不宣称抵抗同账号恶意换路径。

安全LocalFileWriter负责本地专属临时文件、分块append与按意图发布，默认不覆盖；Windows和Linux后端已实现，macOS后端未实现。64 MiB/64 KiB、总120秒、15秒无进展、2秒发布准入等待沿用SFTP预算，阻塞网络I/O在取消或期限到达时通过中断独立SFTP连接收口；原终端连接独立，本地文件系统调用若自身阻塞仍无硬退出保证。published/publication/temporary对下载只描述本地目标和本次临时文件，取消不删除远端或已发布本地目标；contentVerified=false。

下载阶段软件验证与限制见[下载记录](records/B3b_Sftp_Download_2026-10-28.md)，不作为本覆盖阶段重测成绩。

### 显式覆盖与Windows范围

SFTP上传/下载overwrite省略和false等价，true才允许替换原已存在的安全普通目标，同operationId改覆盖值冲突；独立write授权不变。授权的是可信目录中的目标路径替换，不承诺expected-fileId/CAS。初始目标快照与发布前复核能拒绝观察到的目标变化，但最终检查与rename之间仍有竞争窗口。

本地pinPath不执行目录ACL/owner检查，不能声称已强制保证写入者可信。Windows固定原parent，检查fileId/size/mtime/属性后关闭目标检查句柄，再使用原NtSetInformationFile替换；不清目标readonly、不强关外部句柄，不先删再rename。SFTP已有目标仅在posix-rename@openssh.com版本1可用时替换，原目标快照核对type/uid/gid/mode/size/mtime；初始明确不存在仍用hardlink版本1，不允许把后来的竞争创建变成覆盖。

查询失败、拒绝访问或网络错误不等同不存在。新目标沿用临时文件属性，远端仍0600，不自动复制原ACL/执行位。撤权先赢则不发布，发布开始后保留真实成功/unknown结果；取消只清本次temp、不删除目标。成功rename消费temp，远端临时路径消失不证明发布成功，lost ack保持unknown。

本覆盖阶段Windows软件验证已完成，见[覆盖记录](records/B3b_File_Overwrite_2026-10-28.md)。Linux安全本地读写后端已实现，macOS后端未实现，B4由用户实测。托管提权保留unavailable，不作为本轮闭环前置，P8不能在B4前标整体完成。

### UART/ZMODEM发送

`novaterm_upload_serial_file/query_serial_transfer/cancel_serial_transfer`复用原SerialFileTransferController、TransferFileWorker、唯一输入泵和ZMODEM协议owner。MCP只维护有限授权/幂等/事实索引；原worker独占打开、随机读取与销毁安全文件源，不缓存整文件、不重新按路径打开，不新建串口连接或执行器。

发送提供sessionId、epoch、localPath和operationId，remoteName可选；首次上传需确认。调用方独立准备接收端；同主体空闲输入控制可自动衔接，未提交输入或其他主体控制时返回忙碌。上传不自动发送rz、不抢占人工传输。remoteName是单个协议文件名，远端目录和覆盖行为由接收端决定，上传工具不代为选择。

Windows安全源最多64 MiB、64 KiB分块；总期限从首次受理计时，按真实size和baud计算 `max(120000,60000+ceil(size*32000/baud))` 毫秒，硬预算3600000毫秒。超预算在首个协议字节前拒绝。准备、响应/无进展、取消排空各自有有限期限；精确重试返回原transferId且不延长期限，取消只对原Controller操作生效，不转向重连或后来人工任务。

protocolCompleted表示协议会话完整结束；fileConfirmed仅当completedFiles=1，空文件也要文件级确认。ZSKIP可完成协议但未确认文件，状态为skipped；最后握手失败时保留已经取得的fileConfirmed/confirmedBytes。remotePublication固定unknown、contentVerified固定false，发送字节和协议确认不证明远端目录发布、持久落盘或hash正确，取消也不证明远端回滚。

UART发送阶段软件回归已完成，B4实机仍待用户验收，该阶段证据与限制见[UART发送记录](records/B3b_Uart_Send_2026-10-28.md)；[安全源](records/B3b_Uart_File_Source_2026-10-28.md)和[授权目录](records/B3b_Uart_Authorization_2026-10-28.md)保留各自历史证据。B4实机由用户独立验收，软件替身结果不替代真实UART。

两种文件交付都不阻塞终端观察或在GUI执行文件I/O，不自动执行chmod、insmod、烧录或重启。只有独立实际验证内容hash才能认定内容一致，后续动作须使用各自已获准入口。

## 5. 已连接会话

保存目标目录、打开、查询和关闭工具已退役。用户在本机手动建立连接，Agent通过 `novaterm_list_sessions` 发现已连接会话并使用其 sessionId/epoch；认证及主机密钥决策仍由本机用户处理。新就绪连接默认开启读取、输入、托管提权选择和清空权限，同连接内人工撤权不会被刷新恢复。历史实现见[预授权记录](records/B3b_New_Session_Sharing_2026-10-27.md)，不作为当前操作流程。

## 6. 统一 Agent 使用指引

两代握手 instructions、工具描述和用户说明维护同一行为规则，并由契约测试检查语义一致：

1. 本地编辑/编译用 Agent 本地工具，目标板操作用显式 NovaTerm Session，不能混淆两个 Shell。
2. 先列已连接会话并观察，再输入；无会话时请用户在本机连接。
3. 短交互用输入/命令，长任务用异步入口；工具未实现时不推荐虚构调用。
4. 日志游标、当前屏幕、固定 capture 补读各有用途；缺口不证明没有错误。
5. 文本匹配和 UART 静默都不自动证明成功；实际固件标识与功能判据另行核对。
6. 清空、传输和执行分别判断权限与确认；未知结果不换 ID 自动重放，不换工具绕过拒绝。
7. UART发送前独立准备接收端；同主体空闲输入控制可自动衔接；ZSKIP、文件确认、协议结束及远端发布/hash分别判断。

## 7. 验收映射

| 能力 | 复用边界 | 关键回归 |
| --- | --- | --- |
| 文本等待 | 读取/ObservationState、已有屏幕与日志端口 | 旧 PASS、跨页/UTF-8、未换行提示、扫描续接、清空/淘汰/撤权、取消零输入 |
| 长任务 | CommandRuntime/Controller、薄任务索引 | 重复启动、人工接管后停止、断流与重启、UART 无证据、未知结果、记录过期 |
| SFTP上传/下载 | 原FileTransfers/SftpSession队列与worker、安全本地Reader/Writer | 上传确认、跨方向operationId冲突、提前EOF/增长、链接/属性变化、本地发布竞争、取消撤权、临时文件归属 |
| UART发送 | 原Controller/worker、输入泵/ZMODEM owner | 独立权限、lease忙碌、旧代际零写入、原ID取消、首字节前超预算拒绝、ZSKIP/空文件/确认后握手失败 |
| 保存目标 | Profile/Session 创建接线、目标授权 | 重复打开、复用他人会话、串口占用、认证待处理、配置变化、忙碌关闭、旧 epoch |
| 指引 | WireSession/ToolCodec 元数据 | 两代规则冲突、错误权限保证、推荐未实现工具、本地和远端混淆 |

完整调度见[01](01_Phases_and_Acceptance.md)的B3a/B3b，差距见[03](03_Gaps_and_Evidence.md) G14～G17。当前二十工具；Windows与Linux安全文件读写后端已实现，macOS后端未实现。B4尚未整体验收，历史软件验证不代表新一轮实机验收。
