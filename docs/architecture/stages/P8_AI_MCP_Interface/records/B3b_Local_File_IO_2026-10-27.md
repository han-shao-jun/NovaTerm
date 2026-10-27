# B3b 单文件传输：本地句柄 I/O（2026-10-27）

## 范围与设计

基于 de27b10（0.2.91、二十二工具）继续 [06 第4节](../06_Terminal_Workflow_Extensions.md#4-sshsftp-单文件传输)。授权目录只保存意图，现有人工作业 SFTP 数据阶段可能无限阻塞，不能直接作为受限 MCP 传输后端。本批完成可独立验证的 Windows 本地文件句柄边界；不注册没有完整远端后端的工具，不连接设备、不提交/推送、不递增版本。

选择独立 `mcp/files/LocalFileAccess`，只依赖 Qt Core 和系统 API，供后续传输 worker 使用。另一方案是扩张录制模块的 HistoryStorageIo 或直接 QFile 校验后重开；前者职责和权限不同，后者无法约束检查后的替换。本批不改变现有录制/SFTP/UI 行为。沿用 Qt6.8.3/C++17，遵循 effective-cpp 的 RAII、不可复制句柄、noexcept 移动及显式所有权。

接口接受可信调用方已复核的本机授权根及相对文件名，不解码 MCP 路径、不管理身份或授权。调用方必须在启动及发布前复核原 resourceId/revision、客户端身份和当前连接；本批不宣称已交付撤权调度。文件 I/O 均为同步 worker 边界，禁止 GUI/HTTP 线程调用。不复用目录服务做 I/O。

Windows 只从本地卷根按路径打开一次；其后用 `NtCreateFile` 相对已固定父句柄逐级打开目录/文件，均带OPEN_REPARSE_POINT并检查句柄属性，拒绝重解析点。目录句柄需要列目录/读取属性权限且禁止删除共享，阻止父目录被改名/替换；允许写共享以兼容同目录原子发布，不以共享模式阻止所有元数据修改，而是不再通过已检查的字符串祖先重开文件。源文件句柄拒绝写入/删除共享，只接受单链接普通磁盘文件。目标创建同目录独占临时文件，持有 DELETE 权限；`NtSetInformationFile(FileRenameInformation)`相对固定父目录发布，取消清理也不按临时路径重新打开。原生函数不可用时明确返回不支持。仅原子不覆盖发布；已有文件、发布竞争一律拒绝。覆盖和完整传输调度/跨进程发布仲裁留在后续，不以普通覆盖冒充事务。

固定上限：单文件64 MiB、每次读写64 KiB、根路径/相对路径分别1024 UTF-8字节、各最多32段。拒绝绝对/空/点/上级路径、空段、反斜线、控制字符、NT设备/UNC、ADS、保留设备名、尾随点/空格及非法Unicode。只打开已有父目录，不创建目录。写入精确声明长度才可发布；超量、短写或 I/O 失败后不可继续发布。析构清理本次临时文件；成功发布后取消/析构不删除目标。返回有限错误枚举，不携带本机路径；不声称内容hash或断电耐久性已验证。

非 Windows 暂显式 `UnsupportedPlatform`，不退化成存在TOCTOU的实现；其他平台需各自句柄后端和验收后启用。

## 实施计划

任务接口统一使用 `FileIoResult<T>`、`LocalFileReader::open/read/size`、`LocalFileWriter::create/append/publish/cancel`。Reader/Writer均Pimpl、不可复制、可noexcept移动，临时文件所有权由Impl析构回收。新增独立 `novaterm_mcp_file_io` 库和 `novaterm_mcp_local_file_io_tests`，不引入 Widgets/Session/SSH/录制依赖。

- [x] 先写真实文件系统回归和最小失败实现，正常Release构建后记录预期RED；验证普通文件读取、分块与大小限制、路径攻击、硬链接/重解析点、目录替换拒绝。
- [x] 实现逐级目录锁、源句柄和有界读；运行对应测试，确认GREEN。
- [x] 写目标临时文件、精确长度、不覆盖竞争、取消/析构/移动所有权回归，观察RED后实现句柄写入、flush、原子重命名与句柄删除；确认GREEN。
- [x] 默认Release全目标构建，运行新增I/O及现有目录/契约专项；按影响范围验证，不跑无关全套/Core/ConPTY。独立只读复核安全边界和资源寿命。
- [x] 同步P8状态与记录实际命令、失败、通过、未执行范围；不把基础设施测试称作MCP文件交付或B4验收。

## 审查重点

1. 从卷根自上而下通过相对句柄打开，先开子目录再锁父目录或重新按绝对路径打开文件无法排除祖先替换。
2. Windows所有名称解析别名、硬链接和重解析点必须拒绝；错误不带路径。
3. 目标发布须使用仍持有的同一临时文件句柄，竞争失败不能覆盖已有文件。
4. 取消、失败、移动赋值及析构只清理本次所有权，发布成功后不可清理目标。
5. 源读与目标写的大小/分块/状态限制不可通过重复调用绕过；本批未开放协议工具。

## 执行记录

Reader真实RED为2pass/5fail（`build/b3b-local-file-io-reader-red-normal.txt`），实现后7/7；Writer新增用例RED为7pass/5fail（`b3b-local-file-io-writer-red.txt`）。中间发布测试捕获原生重命名字符串终止与Windows目录共享语义问题：对齐/元数据专项2pass/2fail（`b3b-local-file-io-hardening-red.txt`）；最终改为相对目录句柄原生打开/发布，初版完整14/14（`b3b-local-file-io-relative-handle-green.txt`）。前面的沙箱默认TEMP创建失败单列，不算产品RED。

独立只读复核未发现Reader高可信问题，Writer发现只读临时文件可导致清理失败被丢弃。真实文件系统回归2pass/1fail（`b3b-local-file-io-cleanup-red.txt`）；改为同句柄清除只读属性后删除，显式cancel返回失败时保留所有权，完整15/15（`b3b-local-file-io-cleanup-green.txt`）。已发布文件仍不清理，移动赋值/析构/取消只作用于本次临时文件。

早期默认构建沙箱C1902单独保留（`b3b-local-file-io-final-build.txt`）；正常Windows MSVC重建通过（`b3b-local-file-io-final-normal-build.txt`），当时四个相关CTest4/4、0.23秒（`b3b-local-file-io-final-ctest.txt`）。本地库之后进入SFTP上传的Reader生产通路，当前完整构建/相关回归及未验收范围以[上传记录](B3b_Sftp_Upload_2026-10-27.md)为准，不用本批早期成绩替代集成验证。版本0.2.91保持，未提交/推送或连接设备。
