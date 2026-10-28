# SFTP 单文件下载实施计划

> 延续用户授权，以阶段实现、qt-cpp-review行为审查、修复、验证、本地Git提交循环执行；使用subagent-driven-development分配互不重叠的文件任务。既有工作区继续使用，不要求重复确认。

**目标：** 新增novaterm_download_file，复用原查询/取消、SFTP worker和安全本地Writer，允许独立授权的远端普通文件下载到本地授权目录。

**架构：** FileTransfers统一持有上传/下载事实；已有SftpFileUploadRequest尾部追加Direction Upload/Download（默认Upload，保留聚合初始化），已有SftpSession受授权命令分支按方向执行。安全Writer负责本地临时文件及不覆盖发布；不增加生产源文件、不新建连接/线程/注册表。

**技术：** C++17、Qt6.8.3、既有libssh SFTPv3、Windows句柄文件后端；参照06_Terminal_Workflow_Extensions.md第4节。

## 固定边界

- 仅新增一个工具，总30。下载参数与上传九字段同形，relativePath是本地目标相对路径，remoteName是远端单文件名。拒绝绝对路径、遍历、额外参数；默认不覆盖，覆盖下一阶段独立交付。
- 上传对象/历史fixture不改写。query_transfer/cancel_transfer的state明确增加downloading，并精确调整下载适用的描述；新增阶段fixture记录允许变化，原字段及上传输出保持。
- 本地write和远端read独立授权，两边revision、identity、Session完整binding必须匹配；终端权限不推导文件权限。共享原记录/活动预算和单调requestId，跨方向operationId也冲突。
- 64MiB、64KiB、120秒总期限、15秒无进度、2秒发布授权；固定原期限，重试/查询不续期。同步库调用不能保证硬退出，保留现有说明。
- 同一worker中先严格远端根/叶检查，再一次open固定源句柄，按声明大小读、补EOF探测、前后fstat/lstat复核，拒绝链接/设备/不可信属主或可写项。SFTPv3不具openat/NOFOLLOW/inode保证，沿用可信目录属主/管理员边界。
- 本地创建Writer专属临时文件，实际按块append；发布仍经GUI原授权批准和原控制原子状态，回调后/阻塞后复核。取消不删除远端、不删除已发布本地文件；publish/temporary仅反映本地目标。contentVerified恒false。

## 审查重点

1. 旧operationId跨方向重试必须冲突，不能覆盖新目标。
2. 撤权/重连发生在源读取或发布确认期间：停止原任务，不能随新连接继续。
3. 远端短读/增长/链接替换/属性缺失：不得本地发布部分或不可信文件。
4. 取消与Publishing竞争：以真实发布事实为准，不能删除已提交目标。
5. 本地目标竞争创建及源关闭失败：默认不覆盖且原临时文件处置如实报告。

## 任务1：原SFTP下载与安全发布

文件：src/session/SftpFileUpload.h、SftpSession.h/.cpp，tests/mcp/SftpSessionSafetyTests.cpp。不新增生产文件。

- [x] 请求尾部加Direction，原命令分支识别下载；原排队/停止/销毁终态路径复用；公开uploadAuthorizedFile原入口保留兼容，可用方向分派但必须说明其受授权文件语义。
- [x] 将上传现有根检查抽成同文件共用helper，上传原安全/owner断言保持。下载源属性复核及固定句柄读取；在同一类内用窄读取回调抽取下载复制/发布循环供真实Writer测试，生产调用同实现。
- [x] 先新增RED：下载队列方向、共用busy、取消与发布竞争、空文件/分段短读/提前EOF/多字节增长/读错/目标已存在、阻塞回调撤权零发布、源属性不安全；运行目标并保留原输出。
- [x] 实现使RED通过；只根代理统一构建/测试。

## 任务2：应用索引与方向授权

文件：src/mcp/contracts/FileTransferRequest.h、FileTransfers.h/.cpp，tests/mcp/FileTransfersTests.cpp。

- [x] 增加StartFileDownloadRequest（字段同上传），加入FileTransferRequest联合；内部统一请求字段加方向，原upload请求默认Upload。
- [x] authorize按方向判断local read/write与remote write/read；valid同源路径和完整binding复核，replay比较方向；progress按方向uploading/downloading，result格式保持。
- [x] 先RED验证方向权限、跨方向operationId冲突、共享额度/Session busy、发布前撤权、精确重试、取消原operation、不改已有上传断言；实现GREEN。

## 任务3：协议接线与本机面板

文件：ToolCodec、CapabilityRegistry、McpService、McpProtocol、既有MainWindow/SftpPanel及tests/mcp/ContractsTests.cpp、McpTests.cpp、WireSessionTests.cpp、HistoryContractsTests.cpp、interop_check.py、tests/ui/SftpPanelTests.cpp。

- [x] 新工具file.transfer.download及typed解码/路由；仍BackendFileTransferCall，无新backend事件。
- [x] 原starter从既有SftpPanel/Session发送方向，不允许打开新连接或忙碌时抢占；面板状态显示下载事实，不把下载说成上传，终态回调仍原请求id绑定。
- [x] 先RED契约/两代HTTP方向权限、查询/取消、重试；冻结新fixture与精确旧两工具增量，官方SDK2025/rawHTTP2026跑30tools。
- [x] 完成六领域行为审查，修复复测；默认Release构建+相关CTest+完整MCP业务+两代互操作分批记录，不运行无关Core/ConPTY，不连接设备。
- [x] 版本0.2.96，本轮文档/实机清单与AGENTS更新；只提交阶段文件，不推送。随后继续显式覆盖和软件平台收口。


## 实施与真实验证记录

基线e7f1161/0.2.95，目标0.2.96。实现没有新增生产源文件。MainWindow/SftpPanel原starter已完整转交同一个request，添加direction后可直接复用，未改动无收益接口名称；面板真实队列测试增加上传/下载数据行，确认方向、忙碌和原requestId终态路由。公开旧uploadAuthorizedFile命名为兼容保留，头注释明确受授权双向文件请求含义。

下载与上传共用记录/并发额度、单调requestId及两段发布控制。旧上传工具对象保持，query/cancel仅增加downloading枚举，不改历史fixture；新下载fixture独立导出。终端应用ToolRequest仍九项。来源只能是现有SSH SFTP连接的普通单文件，不能接入新连接/凭据。下载目标发布后取消不删除目标，取消只清理本次安全Writer临时文件，contentVerified=false。

| 执行 | 原始结果 | 日志（本地build不提交） |
| --- | --- | --- |
| 新契约/指引RED | 2pass/2fail，3ms | b3b-sftp-download-contract-red.txt |
| 统一RED构建 | 通过 | b3b-sftp-download-red-build.txt |
| SFTP执行安全完整RED | 31pass/32fail，30ms | b3b-sftp-download-owner-red.txt |
| 文件索引完整RED | 13pass/10fail，1223ms | b3b-sftp-download-runtime-red.txt |
| 两代HTTP下载RED | 2pass/2fail，454ms | b3b-sftp-download-http-red.txt |
| 原面板方向传递控制项 | 4/4，29ms；这是已有路径控制，不是RED失败 | b3b-sftp-download-panel-control.txt |
| GREEN定向构建 | 通过 | b3b-sftp-download-green-build.txt |
| 完整契约GREEN | 32/32，50ms | b3b-sftp-download-contract-green.txt |
| SFTP执行安全完整GREEN | 63/63，235ms | b3b-sftp-download-owner-green.txt |
| 文件索引完整GREEN | 23/23，1119ms | b3b-sftp-download-runtime-green.txt |
| 两代HTTP下载GREEN | 4/4，1337ms | b3b-sftp-download-http-green.txt |
| 面板方向传递GREEN | 4/4，32ms | b3b-sftp-download-panel-green.txt |

按qt-cpp-review完成六领域只读行为审查，跳过机械风格。确认一项P2：共用rootGuard抽取时给上传根检查传入cancelled谓词，使上传取消后cleanup必定跳过unlink。恢复上传路径检查的原120秒期限判定；传输/发布仍独立检查取消，下载保持取消敏感。独立复核确认修复。此项为静态审查发现与修复，没有远端lstat/unlink动态替身或真实RED，不用恒真guard测试冒充真实清理验证；已有finalizer回归只覆盖发布/取消状态机。

其余所有权、线程、API/权限、Qt模型契约及资源上限未见高可信新问题。性能审查注明合法短读时完整远端属性复核增加网络往返，不能以本地替身速度推断高延迟链路吞吐；B4须实测。SFTPv3无通用openat/NOFOLLOW/inode身份保证，沿用可信目录owner/admin边界，不声明防御同账号恶意属性伪装。来源超过64MiB在本地临时文件创建前拒绝，totalBytes保持null，不投影不受信远端超限值。

## 命令与验收边界

BuildTools vcvarsall x64后`cmake --build build/Release -j 4`，定向构建使用`--target novaterm_sftp_session_safety_tests novaterm_mcp_file_transfers_tests novaterm_mcp_contracts_tests novaterm_history_contracts_tests novaterm_mcp_tests novaterm_sftp_panel_tests novaterm_mcp_wire_tests`。QTest用`-o build/上述文件名,txt`，HTTP仅新slot为`httpFileDownloadUsesSharedTransferLifecycle`；其他标注完整的测试直接运行全文件。fixture由契约exe的`--write-file-download-fixture tests/mcp/fixtures/file-download-tool-2026-10-28.json`导出。

测试PATH增加Qt6.8.3 bin，QT_PLUGIN_PATH同前缀plugins、QT_QPA_PLATFORM=offscreen，TEMP/TMP=工作区build/b3b-sftp-download-temp，NO_PROXY=*、PYTHONUTF8=1。固定版本提交前默认构建、相关CTest、完整MCP与两代SDK/HTTP互操作已执行，独立结果见下；不以先前成绩替代。

未连接真实SSH/UART、未做其他平台/消毒器/性能验收，未运行无关Core/ConPTY或全套。Windows原安全Writer平台限制保持，显式覆盖与平台软件收尾继续，B4用户实测。


## 0.2.96 提交前收口

| 最终独立验证 | 结果 | 原始日志 |
| --- | --- | --- |
| 正常Windows Release默认全部目标构建 | 通过，exit0 | build/b3b-sftp-download-final-build.txt |
| 相关CTest | 28/28，65.22秒 | build/b3b-sftp-download-final-ctest.txt |
| 完整MCP业务 | 263/263，137.502秒 | build/b3b-sftp-download-final-mcp.txt |
| 官方SDK2025/rawHTTP2026 | 30项输入/输出Schema及原流程/下载拒绝通过 | build/b3b-sftp-download-final-interop.txt |

CTest准确筛选为`ctest --test-dir build/Release -C Release --output-on-failure -R '^novaterm_(mcp_.*|history_contracts|capabilities|capability_entry|resource_sharing|sftp_session_safety|command_safety|session|session_panel|sftp_panel)_tests$' -E '^novaterm_mcp_tests$'`；MCP业务单独完整运行并按上表保存，互操作`python tests/mcp/interop_check.py`。各批验证不拼作一次全套。实机清单DF01～DF07均未执行。

本阶段源码/软件验证完成，本地提交0.2.96，不推送。下一阶段显式覆盖仍以默认不覆盖为基础；平台软件范围继续收口，真实设备、客户端与性能交给用户验收。
