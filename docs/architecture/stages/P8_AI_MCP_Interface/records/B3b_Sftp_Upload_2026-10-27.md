# B3b 受限 SFTP 单文件上传（2026-10-27～28）

## 本轮范围与冻结设计

用户进一步明确各工具不能耦合。最终移除上传对安全关闭策略及终端命令busy的复用，并取消Service对普通终端工具的上传期额外阻塞。上传只复用原SFTP队列，独立核对文件权限和连接；query只读状态，cancel只停止原传输。实际队列占用、文件授权与关闭工具自身的资源busy保护保留，不自动调用其他工具。

基于0.2.91/de27b10及现有工作区本地句柄I/O继续，实现外部Agent的本机产物→获准SSH目录单文件上传、进度查询和取消。按用户“复用终端sftp接口”的要求，直接复用终端SftpPanel已有SftpSession/队列/工作线程；MCP只保留有界事实索引、授权检查与查询取消，不新建连接，不扩张脚本provider。上传不执行文件，Agent随后用现有终端工具执行获准的 `sh`、`insmod` 等并读取结果。

保持旧二十二工具Schema、权限和历史fixture及九项终端请求联合；仅更正目录工具过时的上传不可用说明；末尾追加 `novaterm_upload_file`、`novaterm_query_transfer`、`novaterm_cancel_transfer`，完成后共二十五工具。上传必填sessionId/epoch、resourceId/resourceRevision/relativePath、scopeId/scopeRevision/remoteName、operationId；本机绝对路径/凭据/客户端身份不能由模型提供。查询/取消只接受transferId。上传需要本人当前身份的本地读+绑定原SSH完整连接的远端写，不要求或推导终端权限。无参数授权目录仍只展示授权元数据。

首批仅普通文件，不覆盖。远端使用授权根下单个新文件名，可含构建标识；本机允许安全相对文件路径。最大64 MiB、固定64 KiB分块、每客户端/Session最多一个活动上传、全局两个；受理记录每客户端16/全局64，终态保留10分钟，运行中不淘汰。总体120秒、无进展15秒、发布准入等待2秒；具体底层阻塞行为须核实并记录，不能伪称硬限时。上传与脚本/已有SFTP面板占用互斥检查，不阻塞GUI或终端观察。

本地路径由worker按原授权根的安全目录句柄打开；远端逐段检查目录类型、链接、规范路径、不可信写权限及祖先属主（只接受管理员或授权根属主），操作前/发布前/清理前复核。终端面板SFTP使用相同known_hosts和预期主机指纹；MCP还拒绝旧连接代际。SFTP v3缺少通用openat/目录inode证明，不能承诺对恶意同账号路径改写实现句柄级远端沙箱；检查不通过即拒绝，远端目录管理账号/管理员必须可信。

远端EXCL创建本次随机临时文件，0600。仓库libssh的 `sftp_rename` 会选择覆盖式posix-rename，首批禁止使用它发布。要求服务端支持 `hardlink@openssh.com`：同目录hardlink原子创建新目标、不替换已存在项，再删除本次临时链接。无该能力明确拒绝，不降级覆盖。发布前GUI重新检查身份/授权revision/原连接，worker通过原子阶段转换确定取消与发布的先后；已开始发布保留真实成功或未知事实。

结果分别报告transferId、原sessionId/epoch/operationId、状态、传输字节和总量、published、publication（not_started/published/unknown）、temporary（none/removed/retained/unknown）、errorCode、contentVerified=false。同operationId同参数精确重试只查询原记录；不同参数拒绝，不换ID自动重放；同身份查询保留原事实。取消/撤权/断流在发布前停止，只清理仍能证明属于本次且路径安全的临时文件；发布后不删除目标，清理失败不抹去发布成功。未收到发布确认不能自动重试。

## 执行任务

- [x] 同步当前设计和流程，纠正旧“没有新建会话工具”描述；将UART/ZMODEM单文件上传列为明确后续，下载/覆盖不作本轮前置。
- [x] 契约、三工具Schema、typed事件/路由和二十五工具兼容fixture；有意义的契约RED/GREEN。
- [x] 现有终端SFTP队列/worker扩展（不扩张provider），安全本地读、远端能力检查、不覆盖发布/取消/未知状态回归。
- [x] MCP薄传输索引与GUI组合：授权/连接绑定/幂等/有限记录和并发、进度/发布准入/撤权/断流，真实Session替身HTTP业务回归。
- [x] 正常Windows Release默认全部目标构建、相关CTest、完整受影响MCP业务和两代必要互操作分别记录；不跑无关全套，不连接未授权设备。
- [x] 独立行为复核；记录实际失败及修复，文档状态/实机短步骤与ZMODEM下一起点同步。保留工作区改动，不提交/推送/改版本。

## 后续明确待办

1. UART/ZMODEM单文件上传：复用Session现有SerialFileTransferController/传输输入泵及ZMODEM协议owner；绑定当前UART/epoch和本地读授权，受控接收准备、互斥、取消和未知接收结果另行设计/验收。不将SSH远端目录权限机械套用于UART。
2. SFTP下载、显式覆盖策略、其他平台本地句柄I/O；均不是本轮上传的交付前置。
3. B4真实SSH/UART、客户端、跨平台和性能验收；没有设备/操作授权时不自动连接。

## 本轮真实验证

### 实现及真实RED/GREEN

- 新协议契约2pass/2fail（`build/b3b-sftp-upload-contract-red.txt`）→完整契约26/26（`b3b-sftp-upload-contract-green.txt`）；仅追加三工具fixture，旧Schema/权限保持，历史fixture不改，仅对目录工具说明作精确兼容对照。
- 薄索引/真实Session授权回归2pass/4fail（`b3b-sftp-upload-application-red.txt`）→6/6、182ms（`b3b-sftp-upload-application-green.txt`），覆盖无终端读权限上传、完整绑定、跨客户端、方向权限、精确重试、进度/发布、取消/断流/revision变化与已发布事实保持。网络端口是替身，不是真机传输。
- 原SftpSession安全用例新增受理/发布/未知/取消边界，真实RED11pass/5fail（`build/Release/tests/sftp-authorized-upload-red.txt`）→16/16（`build/b3b-sftp-upload-safety-green.txt`）。这是现有worker的生产发布收尾函数回归，不另建执行器，也不声称完整网络数据流已实机通过。
- 独立只读审查发现祖先0755但属另一用户时可换目录的P1。8组属主矩阵真实RED5pass/5fail（`b3b-sftp-upload-owners-red.txt`），按管理员/授权根属主收紧实际rootGuard，最终安全目标通过。SFTPv3同账号/管理员恶意修改仍须按本文信任边界解释。
- 真实面板复用队列回归2pass/1fail（`b3b-sftp-upload-panel-red.txt`）：上传期间后台目录同步继续入队。补面板操作互斥后，身份回归再取得2pass/1fail（`b3b-sftp-upload-panel-identity-red.txt`）：旧面板仍接受新连接代际。通过相同known_hosts/预期主机指纹建立现有SFTP并固定代际后，最终面板完整21/21、15.607秒（`build/Release/tests/sftp-panel.ctest.txt`）。上传用的正是该面板原有SftpSession，无新连接。
- 传输索引、Service与UI接线独立只读复核未见其他高可信P1/P2；审查者未修改代码或运行设备。前述P1修复由真实RED→GREEN覆盖，不将初次审查写成修复后再次独立审查。

### 本轮构建和完整验证（2026-10-28）

本节是单一职责进一步收口前的一轮；最终源码的成绩见后面的“最终单一职责收口”，不把两轮拼为一次全绿。

正常Windows MSVC/Qt6.8.3 Release默认全部目标构建通过，最终退出0（`build/b3b-sftp-upload-final-build-4.txt`）。文件I/O仅链接实际使用方，TerminalCore不依赖MCP；不改变版本0.2.91。

首轮相关CTest25/26、48.85秒（`b3b-sftp-upload-final-ctest.txt`），唯一HttpMcpServerTests三条显式本地端口bind报“不支持”；原始QTest50pass/3fail（`b3b-sftp-upload-http-diagnostic.txt`）。它们要求实际loopback源端口，夹具改为显式NoProxy，原断言与产品HTTP源码保持；第二轮26/26、49.09秒（`b3b-sftp-upload-final-ctest-2.txt`）。最终依赖收口后，相关CTest **26/26、49.05秒**（`b3b-sftp-upload-final-ctest-3.txt`），不含完整业务目标。

完整MCP业务文件 **247pass/1fail/0skip、122.870秒**（`b3b-sftp-upload-final-mcp.txt`）。唯一失败仍为 `unrelatedGroupOwnedDirectoryIsRejected` 的夹具 `SetNamedSecurityInfoW` 返回Windows1307，未进入生产secureDirectory断言；原断言保留，未跳过/放宽，不称为全绿，也不套用此前246项成绩。本轮新增两代HTTP文件上传权限回归通过，独立专项4/4、920ms（`b3b-sftp-upload-http.txt`）。此业务运行后仅调整独立HTTP测试代理前提和链接依赖，业务源码未再改变；最终26项目标及互操作另外复验，不拼成一轮全套。

官方SDK2025/rawHTTP2026 **二十五工具互操作通过**（`b3b-sftp-upload-final-interop-2.txt`），退出0，检查全部Schema和未获文件权限的拒绝，原终端/历史/等待/任务/连接与重试检查保持。首次502属于回环代理环境（`b3b-sftp-upload-final-interop.txt`）；本轮命令设置 `NO_PROXY=*` 后直连通过，未修改应用代理设置或绕过工具权限。互操作夹具不含真实SSH上传授权，不将其当真机文件交付成绩。

最终退出码及已知业务失败单列于 `build/b3b-sftp-upload-verified-results.json`。构建、26项CTest、248项业务和互操作分别记录；未跑无关全套/Core/ConPTY、跨平台/多客户端性能或真实设备。

```text
cmd /c 'call "C:\Programs\MicrosoftVisualStudio\18\Insiders\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|sftp_panel|command_safety|resource_sharing|session|session_panel|ui_dialog_layout)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-sftp-upload-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin/plugins使用6.8.3，QT_QPA_PLATFORM=offscreen，TEMP/TMP为工作区 `build/b3b-local-file-io-temp`，互操作NO_PROXY=*；SDK依赖沿用build/mcp-test-deps，不增加应用Python依赖。保留原build日志，不提交/推送。简短设备步骤见[U01～U04](../07_Real_Device_Test_Checklist.md#9-sftp上传当前可测其余文件能力后续)，所有真机项仍未执行。

### 当前限制与下一起点

当前上传需要Windows本地句柄Reader、已就绪终端SFTP面板、可信远端目录属主/管理员以及hardlink扩展；遇到能力不足或路径变化明确拒绝。64MiB、分块、记录和并发均有界；120秒/15秒按同步调用间检查，libssh逐字节包滴流或不可中断本地I/O不承诺硬退出。临时文件无法证明原归属/清理失败时保留或报告unknown，不清理陌生文件；发布未知不自动重放。不提供下载、覆盖、chmod或执行，不做内容hash验证。

下一批UART/ZMODEM从现有SerialFileTransferController/输入泵/协议owner开始，先冻结UART当前代际、本地读授权、受控接收准备与独占、取消及未知接收结果，随后实现单文件上传/查询/取消。用户指定设备/操作后再补SFTP与ZMODEM真机验收；不用本轮源码/替身成绩关闭B4。

## 最终单一职责收口（2026-10-28）

按用户明确要求，上传仅上传，query仅查询，cancel仅停止指定传输。删除FileTransfers的外部Idle策略回调，不再复用安全关闭或终端命令运行状态；Service也不因SFTP上传额外阻塞普通终端命令。共享的文件权限/原连接/SFTP队列互斥保留，关闭工具仍按自身资源安全规则拒绝活动传输，不构成工具互相调用。

新增两代真实HTTP回归：只有文件授权、没有终端读取权限且安全关闭策略始终拒绝，上传/查询/取消应独立工作。真实RED **2pass/2fail、394ms**（`build/b3b-sftp-upload-independence-red.txt`），修复后 **4/4、696ms**（`b3b-sftp-upload-independence-green.txt`）；断言关闭策略调用次数为0、查询不再次启动上传、取消只改变原控制句柄，SSH终端输入队列为0。

最后核对还发现目录工具说明及握手保留旧的“上传不可用”文字。指引/严格兼容对照真实RED **2pass/2fail**（`b3b-sftp-upload-guidance-red.txt`）；修正过时文字。旧Schema、权限与全部历史fixture保持；目录工具仅这处说明变化按精确旧→新文本对照，其他字段仍整对象比较，不泛化忽略description。

最终正常Windows Release默认全部目标构建通过（`build/b3b-sftp-upload-final-build-6.txt`）。最终固定源码分别验证：

| 验证 | 本轮实际结果 | 原始日志 |
| --- | --- | --- |
| 相关CTest（排除完整业务） | **26/26，48.00秒** | `build/b3b-sftp-upload-independent-ctest.txt` |
| 完整MCP业务文件 | **249pass/1fail/0skip，124.416秒** | `build/b3b-sftp-upload-independent-mcp.txt` |
| 官方SDK2025/rawHTTP2026 | **25工具Schema与必要互操作通过** | `build/b3b-sftp-upload-independent-interop.txt` |
| 完整契约文件 | **27/27，9ms** | `build/b3b-sftp-upload-independent-contracts.txt` |
| 完整SFTP安全文件 | **24/24，8ms** | `build/b3b-sftp-upload-independent-safety.txt` |
| 两代工具独立性专项 | **4/4，696ms** | `build/b3b-sftp-upload-independence-green.txt` |

业务唯一失败仍是同一外来属主夹具SetNamedSecurityInfoW返回Windows1307，未进入生产断言；原assert保留，不跳过/放宽，不称为全绿。四组新增HTTP行均已进入完整250项文件。各日志分别运行，不累加为全项目全套。退出码见 `build/b3b-sftp-upload-independent-results.json`；build/ctest/interop/contracts/sftp/independence均0，业务1明确保留。

最终独立只读增量复核未见高可信P1/P2：构造/调用一致，发布仍复核文件权限和完整绑定，取消与幂等边界保持，没有第二连接/执行器。没有连接设备，所有真机清单项未执行；版本0.2.91保持，未提交/推送。文档链接检查无新增缺失，AGENTS两条HEAD已有历史缺失单列（`build/b3b-sftp-upload-doc-check.json`），diff空白检查通过。上述命令与Qt/TEMP/NO_PROXY前提沿用前节，仅输出前缀改为本节日志名。

## 0.2.92 提交前收口（2026-10-28）

用户要求创建本地Git提交，按每提交PATCH递增约定将0.2.91→0.2.92，包含本地句柄I/O、复用终端原SFTP的上传/查询/取消、工具独立性收口、回归及当前文档/实机步骤。二十五工具，旧Schema/权限与历史fixture保留，仅精确更正目录工具过时说明。不推送，不纳入build产物、日志或本机辅助文件，不改机械风格，不连接真实设备；下载、覆盖、UART/ZMODEM及B4仍待后续。

用户随后明确“直接创建git提交”，本次不再重复运行测试。前节0.2.91的相关CTest26/26、完整业务249pass/1fail（唯一外来属主夹具Windows1307）、27项契约/24项SFTP安全及两代25工具互操作均作为该轮真实成绩保留，不改写为0.2.92重测成绩。版本调整仅改根project版本，不新增功能或机械改动；build日志和本机辅助文件不纳入提交。

版本调整后已启动的正常Windows Release默认目标构建完成，退出0（`build/b3b-sftp-commit-0.2.92-build.txt`）；没有再次运行测试。用户追加授权推送到Gitee，仅推送该远端，不改其他远端跟踪设置。
