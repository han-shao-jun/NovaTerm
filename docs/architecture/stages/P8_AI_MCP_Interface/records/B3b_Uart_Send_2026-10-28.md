# UART/ZMODEM MCP 单文件发送

基于139aa9d/0.2.94，阶段目标版本0.2.95。用户要求持续实施、行为审查、修复后本地提交，真机由用户最终测试。本阶段新增独立upload_serial_file/query_serial_transfer/cancel_serial_transfer，完成后29工具；旧26工具Schema/fixture保持。复用原Session/Controller/worker/ZMODEM，不新增串口连接、执行器或输入旁路，不自动rz/释放MCP lease。

用户追加要求复用现成接口、避免过度设计和大量新源文件：UART请求类型并入已有FileTransferRequest.h，不新增零碎契约头；运行期只新增必要的SerialFileTransfers薄适配两文件及专项测试。SFTP后续能力继续扩展现有SftpSession/FileTransfers。旧26工具Schema与历史fixture保持；仅精确更正UART目录说明中“发送尚不可用”的过时文字，不泛化忽略description。

## 冻结设计与接口

### 原操作身份与真实I/O隔离

- `SerialTransferResult { quint64 transferId; bool protocolCompleted; SerialTransferProgress progress; FileTransfer::Error error; }` 为不可变完成快照；新增Controller `resultReady(const SerialTransferResult&)`，旧finished信号保持。成功/失败都保留已有确认进度，ZSKIP保持completedFiles=0。
- `start(request, quint64* acceptedId=nullptr)` 在任何同步通知前写入原操作ID，拒绝时为0。新增 `bool cancel(quint64 expectedId)` 和 `bool abort(quint64 expectedId,const QString&)` 只作用于原活动ID，旧无参API保持。本次finish整个清理/通知期间拒绝新start，start在同步通知后检查原操作仍活动；旧启动不能驱动回调中新建任务。
- `SerialTransferRequest::admission` 为可选 `std::function<bool()>`，仅在GUI线程用于MCP持续授权复核；首次协议启动、文件动作提交/处理和payload写入前复核，取消收尾CAN不依赖已撤销权限。回调后检查原epoch与对象仍有效，避免重入污染新任务。
- SerialIoState的queueWrite/requestClear增加默认0的expectedGeneration参数，在原mutex内复核；SerialTransport的tryWriteBounded/clearPendingOutput/pendingWriteBytes转交该参数。现有普通终端调用保持。
- Session reserve捕获本次Session及真实I/O generation，writer/connected/activate/pending/clear/disconnectRequired均不得转向新代际；普通payload和取消CAN走同一原writer，旧取消不能清新连接队列或断开新连接。

### MCP薄索引

- 新 `SerialFileTransfers` 只管理GUI线程有界事实与授权，生产直接取原Session.serialFileTransfer()；可注入Controller解析窄端口用于离线测试，不造第二worker。
- 构造 `SerialFileTransfers(AccessStore&,SessionDirectory&,FileResourceAdapter&,SerialFileTargetAdapter&,QString instance,QObject* parent=nullptr,ControllerResolver={},Now={})`；`ControllerResolver=std::function<SerialFileTransferController*(TerminalSession&)>`，`Now=std::function<qint64()>`单调时钟窄端口，默认QElapsedTimer。`call(client,identity,connection,const SerialFileTransferRequest&) -> Result<QJsonObject>`，另有disconnect(connection)/stop()/busy(session)。Service持有并接入关闭busy保护，不影响普通观察或代替终端工具。
- 结构 `StartSerialFileUploadRequest { SessionReference reference; QString resourceId,resourceRevision,relativePath,targetId,targetRevision,remoteName,operationId; }`；QuerySerialFileTransferRequest/CancelSerialFileTransferRequest仅transferId，联合为SerialFileTransferRequest。
- 逐次验证客户端identity、本地read资源及revision、独立UART授权targetId/revision、完整SerialFileBinding/原Session。目录回调后重读纯授权，持续admission复核；普通终端权限不推导文件发送权。
- 每客户端16/全局64条保留记录，每客户端/Session一个活动、全局两个；终态10分钟，活动不淘汰。identity+operationId精确重试返回原事实，不同参数冲突。query/cancel只定位原记录和原Controller ID，不跟随换绑或后来人工任务。
- source工厂仅捕获路径与共享atomic停止标志，在原worker内LocalFileReader.open/readAt；64MiB、64KiB限制保持，取消不再读源。不回传源路径和本地化错误文本。
- 总期限从首次受理计时，真实size已知后按 `max(120000,60000+ceil(size*32000/baud))` 毫秒计算（32位/字节为编码与帧余量），硬预算3600000ms；超过预算在首个协议字节前拒绝。准备仍走原5秒期限，原响应/无进展和取消排空期限保持。100ms生命周期检查和admission均可请求原任务停止，重试不延长期限；取消排空可能在期限后有限继续，不能承诺驱动阻塞硬退出。
- 结束事实：`protocolCompleted`表示协议会话完整结束；`fileConfirmed`仅当completedFiles==1，空文件也按文件计数确认；成功结束但0文件为skipped。即使最后握手失败，已有fileConfirmed/confirmedBytes仍保留。远端路径/覆盖/持久落盘不受本机证明，`remotePublication="unknown"`、`contentVerified=false`，取消不宣称回滚。

### 工具契约

- `novaterm_upload_serial_file` 必填sessionId/epoch、resourceId/resourceRevision/relativePath、targetId/targetRevision、remoteName、operationId。ID为规范非空UUID；relativePath有界相对路径，remoteName为最多255 UTF-8字节单文件名，无参数可改串口配置、远端目录或执行命令。
- `novaterm_query_serial_transfer`/`novaterm_cancel_serial_transfer`仅接受transferId，保持SFTP三工具原对象不变。
- data共同字段：instanceId、transferId、sessionId、epoch、operationId、protocol=zmodem、state（accepted/sending/cancel_requested/completed/skipped/cancelled/failed/unknown）、confirmedBytes、totalBytes可空、completedFiles、protocolCompleted、fileConfirmed、remotePublication=unknown、contentVerified=false、timeoutMs可空、errorCode可空。

## 实施任务与验证

1. [x] Controller原操作ID/不可变结果/授权guard，Session与SerialIoState原代际写/清边界；确定性RED覆盖同步start/finish重入、旧代际payload/CAN/clear零作用、新队列保留。
2. [x] SerialFileTransfers索引与安全源适配，独立权限、原ID取消、超时预算、ZSKIP/空文件/确认后失败/销毁事实；真实Controller/ZMODEM替身与源文件回归。
3. [x] typed协议/Service/工具Schema/29工具fixture和两代互操作；真实HTTP权限独立/拒绝/重试/查询取消回归。
4. [x] 正常Release构建、受影响Session/串口/协议/MCP相关目标及完整业务分别验证；qt-cpp-review六领域只读复核，有问题真实RED修复；同步文档和实机清单后本地提交，不推送。

## 结果账本

### 开发与审查回归

| 本轮执行 | 原始结果 | 日志（工作区build，不提交） |
| --- | --- | --- |
| 初始RED构建 | 首次C4100未用兼容stub参数、第二次测试符号serverInstructions不存在；分别修正编译夹具后第三次构建成功，不把编译失败计作行为RED | b3b-uart-send-red-build.txt、-2.txt、-3.txt |
| 原I/O代际专项RED | 3pass/4fail，192ms | b3b-uart-send-io-red.txt |
| Controller身份/重入/授权RED | 3pass/5fail，155ms | b3b-uart-send-owner-red.txt |
| 独立同步销毁RED | 0xc0000005访问异常，exit=-1073741819，仅init通过、没有Totals；不是普通断言失败 | b3b-uart-send-deletion-red.txt |
| 运行期完整RED | 16pass/17fail，1660ms | b3b-uart-send-runtime-red.txt |
| 新工具与指引RED | 2pass/2fail，4ms | b3b-uart-send-contract-red.txt |
| 首轮GREEN构建 | 成功；Session、串口Controller、UART运行期、契约、Wire目标 | b3b-uart-send-green-build.txt |
| Controller专项GREEN（含同步销毁） | 9/9，182ms | b3b-uart-send-owner-green.txt |
| 原I/O代际专项GREEN | 7/7，245ms；保留QTemporaryDir清理warning | b3b-uart-send-io-green.txt |
| UART运行期完整首轮GREEN | 34/34，4906ms | b3b-uart-send-runtime-green.txt |
| 完整契约GREEN | 30/30，90ms | b3b-uart-send-contract-green.txt |
| 两代真实HTTP新增接线 | 4/4，1997ms；新增正向回归未取得独立RED | b3b-uart-send-http-green.txt |
| 审查预算契约回归RED | 2pass/1fail，127ms | b3b-uart-send-budget-red.txt |

Controller同步删除使用独立进程，仅进程级SetErrorMode抑制Windows崩溃弹窗。真实Session默认resolver/MCP lease用例为GREEN阶段新增，没有独立RED。HTTP使用真实Service/Controller/worker和缺失安全源，验证两代路由、独立授权、重试/冲突、跨客户端拒绝、终态查询/取消与零串口输出，不声称HTTP验证了真实设备传输。

按qt-cpp-review六领域只读复核，跳过机械风格。发现并复现一项P2：低波特率大文件预算拒绝后仍输出超过3600000ms的估算timeoutMs，违反输出Schema。修复为只有符合预算的期限才写入；预算拒绝timeoutMs为null，查询、精确重试和取消保持一致。其余模型契约（本次无模型结构变更）、所有权、线程、授权/错误处理和有界性能审查未见高可信新增问题。修复后的独立复核确认问题已解决；统一验证见下，不套用上表较早成绩。

### 构建与运行命令

使用BuildTools vcvarsall x64后执行`cmake --build build/Release -j 4`；定向构建附加`--target novaterm_session_tests novaterm_serial_file_transfer_tests novaterm_mcp_serial_file_transfers_tests novaterm_mcp_contracts_tests novaterm_mcp_wire_tests`。测试PATH增加Qt6.8.3 bin，QT_PLUGIN_PATH指向同前缀plugins，QT_QPA_PLATFORM=offscreen，TEMP/TMP指向工作区build/b3b-uart-send-green-temp。QTest使用`-o build/上述日志名,txt`保留完整结果。新工具fixture由契约可执行文件`--write-serial-transfer-fixture tests/mcp/fixtures/serial-transfer-tools-2026-10-28.json`导出，旧26项仅允许明确列出的目录description一句修订。

无真机、无跨平台验收结论；剩余阶段见[软件收口账本](../08_Software_Closeout.md)。


### 0.2.95 提交前收口

用户授权每阶段完成审查、修复和验证后创建本地提交；本阶段版本0.2.94→0.2.95。新增29工具中的UART三项，没有连接设备或推送。

| 本轮最终验证 | 结果 | 原始日志 |
| --- | --- | --- |
| 正常Windows Release默认全部目标构建 | 通过，exit0 | build/b3b-uart-send-final-build.txt |
| 受影响Controller完整QTest | 188/188，104.568秒 | build/b3b-uart-send-final-controller.txt |
| 相关CTest首轮 | 28/29，65.09秒；唯一失败为历史工具总量仍按26计算 | build/b3b-uart-send-final-ctest.txt |
| 历史契约失败原文件 | 6pass/1fail，12ms；旧对象比较保持 | build/b3b-uart-send-history-count-failure.txt |
| 修正计数后定向构建与CTest | 1/1，0.10秒 | build/b3b-uart-send-history-count-build.txt、build/b3b-uart-send-history-count-green.txt |
| 完整MCP业务 | 261/261，139.176秒 | build/b3b-uart-send-final-mcp.txt |
| 官方SDK2025/rawHTTP2026 | 29项完整输入/输出Schema、目录、文件拒绝和原终端/任务流程通过 | build/b3b-uart-send-final-interop.txt |

相关CTest包含MCP标签25项（业务另跑）与Session、SessionPanel、SftpPanel、串口传输UI。UART预算拒绝查询/重试/取消回归在该轮运行期目标通过。首轮28/29与修正后1/1是分段结果，不拼作一次29/29或全套成绩。构建后只补头文件语义注释、修正测试计数与文档，没有改变生产行为；计数目标已重新构建复验。

CTest准确筛选：`ctest --test-dir build/Release -C Release --output-on-failure -R '^novaterm_(mcp_.*|history_contracts|capabilities|capability_entry|resource_sharing|sftp_session_safety|command_safety|session|session_panel|sftp_panel|serial_file_transfer_ui)_tests$' -E '^novaterm_mcp_tests$'`；计数修正复验`-R '^novaterm_history_contracts_tests$'`。完整业务和Controller分别直接执行同名exe并`-o`保存上述文件；互操作`python tests/mcp/interop_check.py`。Qt环境同上，NO_PROXY=*，PYTHONUTF8=1。

未跑无关全套/Core/ConPTY、其他平台或消毒器；原Windows安全本地文件后端限制保持。实机清单已补UF01～UF08且全部未执行。下一软件阶段为SFTP单文件下载，其后显式覆盖与平台收尾；B4真实设备/真实客户端/性能由用户最终验收。
