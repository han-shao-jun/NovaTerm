# SFTP 单文件显式覆盖实施计划

> 延续用户授权的实现→qt-cpp-review行为审查→修复验证→本地提交流程，不改机械风格。目标0.2.97，以0.2.96下载提交为基线。使用现有文件，不新增生产执行器/源文件。

**目标：** 给upload_file/download_file增加可选overwrite布尔参数，默认false，保留30工具；仅在明确意图与原文件write授权下替换授权路径中的安全普通文件。

**规格：** 06_Terminal_Workflow_Extensions.md第4节；对应当前LocalFileWriter与SftpSession发布边界。原查询/取消/授权不扩权。

## 冻结语义

- overwrite省略和false规范化相同；同operationId改变true/false冲突。两开始请求与SftpFileUploadRequest尾部加bool默认false，原聚合初始化和旧输入仍接受。
- 请求授权的是该目标路径的替换，不承诺expected-fileId条件替换/CAS。发布前复核原目标身份/属性，观察到替换/删除/类型变化则拒绝；最后检查与rename之间仍有竞争窗口。只应对可信写入者的目录使用显式覆盖，本地现有pinPath不执行ACL/owner校验，不能声称已经强制实现此信任前提。
- 初始明确不存在：即使overwrite=true也用原不覆盖原子发布；后续竞争创建拒绝，不能临时升级成覆盖。未知/拒绝访问/共享冲突/网络查询错误不得等同不存在。
- 初始存在且overwrite=true：只允许普通文件，拒绝重解析点/链接/目录/设备/多硬链接（本地）、缺失属性/不可信属主及group/other可写（远端）。不清readonly、不强关句柄，不自动复制旧目标属性、ACL、执行位；远端新temp仍0600。
- Windows固定原parent，捕获原目标fileId/size/mtime/属性，临近发布复核并关闭目标检查句柄后使用现有NtSetInformationFile(FileRenameInformation=10,ReplaceIfExists=TRUE)。经典rename不保证可替换仍打开的目标，不增加Ex/POSIX兼容分支。依据：https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_rename_information 。
- SFTP已有目标仅在posix-rename@openssh.com版本1支持时用既有sftp_rename。初始不存在仍hardlink@openssh.com版本1。远端目标快照用普通type/uid/gid/mode/size/mtime复核；SFTPv3无inode/CAS，同账号恶意替换限制保持。
- 发布仍经原GUI批准与Publishing原子状态；撤权先赢则不发布，发布后事实保持；远端响应不明为unknown，临时路径消失不是成功证据。取消仅清理本次temp，从不删除目标。rename成功消费temp，temporary=removed；失败/未知清理不能误删目标或未知新路径。

## 任务1：本地Writer覆盖

文件src/mcp/files/LocalFileAccess.h/.cpp、tests/mcp/LocalFileAccessTests.cpp。
- [x] create(trustedRoot,relativePath,expectedBytes,bool overwrite=false)；检测明确不存在与其他错误，按旧默认行为保持不覆盖。
- [x] 为既有安全目标捕获固定parent和目标身份，最终复核后一次原子replace；不允许先删除目标再rename。
- [x] RED→GREEN：默认拒绝、true成功、取消保旧/发布后取消保新、初始不存在后竞争创建、目标变更/删除/链接/目录、外部句柄/readonly拒绝、异常不得删目标。

## 任务2：SFTP覆盖与下载传参

文件src/session/SftpFileUpload.h、SftpSession.h/.cpp、tests/mcp/SftpSessionSafetyTests.cpp。
- [x] worker请求尾部overwrite=false；下载向LocalFileWriter传递，受授权队列含参数。
- [x] 复用原上传root/临时归属检查；true才探测原目标，明确notfound与查询错误区分；基于实际扩展和原目标状态选择hardlink或posixrename，无fallback先删。
- [x] RED→GREEN：能力分支、未知目标拒绝、普通/属性目标复核、rename成功/拒绝/unknown、temp被消费的结果、取消/撤权仍能清理原ownedtemp。

## 任务3：索引、Schema、文档与提交

文件FileTransferRequest、FileTransfers、ToolCodec、McpProtocol、ContractsTests、FileTransfersTests、McpTests、interop_check.py与现行文档。
- [x] 两typed请求尾部overwrite规范化，record比较包含bool；独立write授权保持，不增加权限推导。
- [x] 仅两输入Schema增加可选boolean，上传/下载描述说明路径替换与非CAS信任前提；旧fixture保留，新阶段fixture精确验证增量。
- [x] RED→GREEN检验省略/false等价、true传参、同ID改覆盖冲突、两代HTTP授权与发布事实，30工具互操作。
- [x] 六领域只读行为审查、修复、默认Release构建、相关CTest、完整MCP业务及两代互操作，分批保留真实结果。
- [x] 更新实机清单的覆盖/目标竞争/未知结果项，全部未执行；版本0.2.97本地提交，不推送。平台范围随后按用户偏好收口。


## 实施与审查结果

基线b71af72/0.2.96，当前工作区0.2.97。生产只扩展既有LocalFileAccess、SftpSession、FileTransfers与协议文件，没有新源文件、线程、连接或执行器。两开始请求及worker参数尾部增加默认false；省略/false精确重试一致，切换覆盖意图冲突。工具数保持30。

Windows初始probe现在只认STATUS_OBJECT_NAME_NOT_FOUND，其他查询失败拒绝；已有目标捕获volume/fileId/size/mtime/attrs，发布前复核，临时句柄仍为原本次所有物。SFTP仅true额外探测目标：原存在安全目标采用明确的posix-rename1支持，原不存在只采用hardlink1；模式固定，不因后续文件消失/出现转向另一种发布。已确认rename消费临时文件，不再按旧临时路径清理；unknown保留不确定性，不从路径缺失反推成功。下载复用同一Writer覆盖实现。

| 本轮执行 | 结果 | 日志（本地build，不提交） |
| --- | --- | --- |
| 统一RED构建 | 通过 | b3b-file-overwrite-red-build.txt |
| 本地I/O完整RED | 24pass/13fail，219ms | b3b-file-overwrite-local-red.txt |
| SFTP安全完整RED | 70pass/20fail，84ms | b3b-file-overwrite-sftp-red.txt |
| 文件索引完整RED | 23pass/4fail，1777ms | b3b-file-overwrite-runtime-red.txt |
| 新覆盖契约RED | 2pass/2fail，11ms | b3b-file-overwrite-contract-red.txt |
| 两代/双向HTTP RED | 2pass/4fail，1187ms | b3b-file-overwrite-http-red.txt |
| GREEN定向构建 | 通过 | b3b-file-overwrite-green-build.txt |
| 本地I/O首轮GREEN | 36pass/1fail，254ms；delete-pending夹具的内容复核遇共享冲突 | b3b-file-overwrite-local-green.txt |
| 修正夹具后本地I/O完整 | 37/37，209ms | b3b-file-overwrite-fixture-build.txt、b3b-file-overwrite-local-final.txt |
| SFTP安全完整GREEN | 90/90，125ms | b3b-file-overwrite-sftp-green.txt |
| 文件索引完整GREEN | 27/27，1265ms | b3b-file-overwrite-runtime-green.txt |
| 完整契约GREEN | 34/34，42ms | b3b-file-overwrite-contract-green.txt |
| 两代/双向HTTP GREEN | 6/6，1702ms | b3b-file-overwrite-http-green.txt |

夹具首轮失败发生在产品已拒绝delete-pending目标、测试已撤销删除标记之后：仍持有DELETE access原生句柄，QFile读取不shareDelete导致共享冲突。修正为结束内层RAII作用域关闭该句柄后再读取原内容，原拒绝/内容/无临时文件断言全部保持，没有修改生产代码或放宽断言。

qt-cpp-review六领域独立只读行为审查完成，跳过机械风格，未发现>80高可信新增问题。已追踪仓库libssh0.12.2 sftp_rename分支确认版本1 posix扩展；本地Native rename/文件保护有实际Windows测试，远端策略/最终发布状态机为替身测试，不代表真实远端rename、响应丢失或文件替换验收。

## 构建命令与范围

BuildTools vcvarsall x64后`cmake --build build/Release -j 4`；定向构建使用`--target novaterm_mcp_local_file_io_tests novaterm_sftp_session_safety_tests novaterm_mcp_file_transfers_tests novaterm_mcp_contracts_tests novaterm_mcp_tests`。本地夹具修正仅定向重建local_file_io目标复验。QTest完整文件直接运行对应exe，契约RED指定fileOverwriteIsAnOptionalBoolean，两代HTTP指定httpFileOverwriteRequiresExplicitIntent；每次`-o build/上述文件名,txt`保留原始结果。覆盖fixture使用`--write-file-overwrite-fixture tests/mcp/fixtures/file-overwrite-tools-2026-10-28.json`独立导出，历史fixture不改写。

PATH增加Qt6.8.3 bin，QT_PLUGIN_PATH同前缀plugins，QT_QPA_PLATFORM=offscreen；TEMP/TMP=工作区build/b3b-file-overwrite-temp，NO_PROXY=*，PYTHONUTF8=1。提交前正常Release默认构建、相关CTest、完整MCP业务和两代互操作已完成，实际结果单列如下，不套用上述较早成绩。

平台问题已异步询问并给予回复时间，尚无用户回复；本轮按既有Windows工作区收口，已在会话明确说明此假设。Linux/macOS安全本地文件后端是未实现后续，不能称为仅未测试；托管提权adapter保持明确不可用且按03不作为首个闭环前置。B4真实设备、客户端、跨平台运行与性能由用户执行，软件完成不等于P8/BSP闭环最终验收。


## 0.2.97 最终提交与Windows软件交付

| 最终独立验证 | 结果 | 原始日志 |
| --- | --- | --- |
| 正常Windows Release默认全部目标构建 | 通过，exit0 | build/b3b-file-overwrite-final-build.txt |
| 相关CTest | 28/28，66.27秒 | build/b3b-file-overwrite-final-ctest.txt |
| 完整MCP业务 | 267/267，141.237秒 | build/b3b-file-overwrite-final-mcp.txt |
| 官方SDK2025/rawHTTP2026 | 30工具完整输入/输出Schema、显式覆盖参数拒绝与原流程通过 | build/b3b-file-overwrite-final-interop.txt |

CTest筛选：`ctest --test-dir build/Release -C Release --output-on-failure -R '^novaterm_(mcp_.*|history_contracts|capabilities|capability_entry|resource_sharing|sftp_session_safety|command_safety|session|session_panel|sftp_panel)_tests$' -E '^novaterm_mcp_tests$'`。完整业务单独执行exe并保存上表文件，互操作`python tests/mcp/interop_check.py`。各批分别记录，不拼为一次全套；未重跑无关Core/ConPTY、其他平台、消毒器或真实设备。

0.2.97本地提交收口，Windows当前B0～B3b软件范围完成并可进入用户实机测试。最终六领域审查未见高可信未修复问题，相关文档旧状态矛盾已统一；无新生产源文件、无新执行器。OF01～OF08与原SF/DF/UF检查均仍未执行。P8/BSP闭环最终验收尚未完成，不以本机替身和协议成绩关闭B4。

明确后续：Linux/macOS安全本地文件Reader/Writer代码未实现；托管提权adapter按现行首闭环范围继续不可用。上述不是本轮已完成的软件能力，也不能转写成仅待用户真机测试。平台范围可在用户后续明确要求时继续开发，不重做已提交Windows阶段。
