# UART/ZMODEM 安全文件源前置子批

**目标：** 让现有串口 worker 能从安全打开、同一生命周期持有的文件源按偏移读取，满足 ZMODEM 重传/续传；本轮完整交付该前置通路及替身回归，保持现有二十五工具。

**依据：** [06 工作流](../06_Terminal_Workflow_Extensions.md) 第4节、[上传记录](B3b_Sftp_Upload_2026-10-27.md) 的UART下一工作项，以及 SerialFileTransferController/TransferFileWorker/ZmodemEngine 当前源码。基于632bec0/0.2.92，保留上一轮未提交的SFTP两项修复。

## 设计与边界

现有ZMODEM在ResumePosition后再次发出ReadAt，不能把授权根拼接成路径交给QFile重新打开，也不能只用顺序Reader。本轮选择：安全Reader补同句柄readAt，串口门面增加worker内创建并独占持有的单文件源。整文件缓存会扩大内存与取消成本，路径重新打开会丢失安全句柄约束，故均不采用。

- LocalFileReader新增 `FileIoResult<QByteArray> readAt(quint64 offset, qsizetype count)`。只读原固定句柄；count须1～65536，offset/count用减法检验不越过已观察大小，拒绝溢出/短读。readAt不推进顺序read的逻辑游标，read在其consumed位置继续；原64MiB与目录/文件锁保持，非Windows仍明确拒绝。
- `src/session/transfer/SerialTransferSource.h` 定义不依赖MCP的Qt门面源：虚析构、`quint64 size() const noexcept`、`bool readAt(quint64, qsizetype, QByteArray&, QString&)`。`SerialTransferSourceSpec`含name和 `std::function<std::unique_ptr<SerialTransferSource>(QString&)> open`。
- SerialTransferRequest新增可选source，仅用于ZMODEM单文件发送，不能混用files/destination/expectedSize；空工厂/非法名在打开前拒绝。现有手动files/QFile路径保持。
- TransferFileWorker只在run内调用open一次，以局部unique_ptr持有并在worker线程销毁；准备成功后仅把文件名/真实size传回GUI，ReadAt按文件0、范围和精确长度复核。取消后不发新动作；同步打开/读取返回后的旧结果仍按现有epoch丢弃，不在GUI等线程。
- 真实LocalFileReader适配放在集成测试夹具，串口生产库保持不依赖MCP；本轮不预先添加无生产调用者的MCP UART执行索引/授权框架。

后续公开UART工具必须先补独立、绑定原串口连接的发送授权及只读发现，不能借终端权限或SSH scope。接收准备由外部Agent独立执行并显式release已有输入lease，上传不自动发送rz或抢占。ZSKIP可使协议会话结束但completedFiles为0；后续必须区分协议结束、单文件确认、远端发布/hash事实。串口总期限需单独按速率冻结，不套用SFTP120秒。本轮不开放UART MCP上传、不连接真实设备。

## 实施计划

使用writing-plans/TDD和effective-cpp；沿用当前授权工作区，不创建提交/推送或改机械风格。测试按项目要求选相关目标，不跑无关全套/Core/ConPTY。

### 1. 安全Reader随机读取

文件：`src/mcp/files/LocalFileAccess.h/.cpp`、`tests/mcp/LocalFileAccessTests.cpp`。

- [x] 先补可执行RED：前进/回退重读、read→readAt→read、越界/溢出/块预算、空文件、原句柄防替换。
- [x] 实现readAt及顺序游标保持；运行完整novaterm_mcp_local_file_io_tests，记录原始RED/GREEN。

### 2. 串口worker源工厂

文件：新增`src/session/transfer/SerialTransferSource.h`；修改SerialTransferTypes、TransferFileWorker、SerialFileTransferController及`tests/session/SerialFileTransferTests.cpp`。

- [x] 先补准入/工厂调用/随机读/失败和取消生命周期RED，工厂仅在线程内运行。
- [x] 接入单源准备与ReadAt，保持原输入泵、lease、协议owner和手动路径；原发布/取消断言保持，完整串口目标另记录。

### 3. 安全Reader与真实ZMODEM集成

文件：新增`tests/mcp/SerialFileSourceTests.cpp`与对应CTest目标，链接现有mcp_file_io和serial_transfer。

- [x] 用真实LocalFileReader源工厂、真实Controller/worker/ZMODEM对端替身核对文件内容和completedFiles；覆盖随机重读、取消后迟到准备结果及安全打开失败零协议输入。
- [x] 正常Release默认全部目标构建；相关CTest、完整MCP业务、两代25工具互操作与受影响串口目标分别记录；独立行为复核、文档同步与链接检查。

## 验证记录

新API先提供声明/拒绝式占位，以取得可执行行为RED；没有把缺少声明的编译错误作为产品失败证据。

| 轮次 | 实际结果 | 原始日志 |
| --- | --- | --- |
| 安全Reader完整RED | **13pass/6fail，125ms** | `build/b3b-uart-source-reader-red.txt` |
| 串口source专项RED | **20pass/10fail，13ms** | `build/b3b-uart-source-worker-red.txt` |
| 安全Reader/ZMODEM集成RED | **2pass/5fail，7ms** | `build/b3b-uart-source-integration-red.txt` |
| 安全Reader完整GREEN | **19/19，142ms** | `build/b3b-uart-source-reader-green.txt` |
| 串口source专项GREEN | **30/30，1.378秒** | `build/b3b-uart-source-worker-green.txt` |
| 安全Reader/ZMODEM集成GREEN | **7/7，2.163秒** | `build/b3b-uart-source-integration-green.txt` |

RED中非法/混用准入对照行保持通过；功能行因拒绝占位失败。首次集成测试构建因测试文件遗漏命名空间闭合发生C1075（`build/b3b-uart-source-worker-red-build.txt`），修正后重建通过（`build/b3b-uart-source-worker-red-build-2.txt`）才运行上述行为RED，该编译错误不计产品缺陷。GREEN退出码均0（`build/b3b-uart-source-green-results.json`）。

随机重读由真实Controller/worker/ZMODEM驱动0→512→1536偏移并核对有效负载；本机Reader分别验证同句柄前后读和顺序游标互不干扰。独立集成目标`novaterm_mcp_serial_file_source_tests`把真实安全Reader接入现有worker和真实ZMODEM对端替身，验证二进制/空文件、ZSKIP的completedFiles=0、安全打开失败零协议输出、工厂阻塞后abort及源在worker销毁。非Windows安全Reader仍未实现，该集成的本机成功路径明确按平台跳过，不能套用Windows成绩。

修复/实现后独立只读复核未见高可信Critical/Important新问题；检查固定句柄范围/游标、线程所有权、失败零输出、取消/旧epoch、ZSKIP与默认手动路径兼容。审查者未修改代码、构建、测试或连接设备。

本轮源码/替身结果不代表真实UART、远端目录/覆盖或文件落盘已经验收。当前仍二十五工具、版本0.2.92，无自动rz/输入lease抢占，无新增MCP UART权限或入口。

## 固定源码完整验证

正常Windows MSVC BuildTools/Qt6.8.3 Release默认全部目标构建通过，退出0（`build/b3b-uart-source-final-build.txt`）。本轮固定源码各批结果独立记录：

| 验证 | 实际结果 | 原始日志 |
| --- | --- | --- |
| 相关CTest（包含新集成及串口UI，完整业务和完整串口另跑） | **28/28，59.50秒** | `build/b3b-uart-source-final-ctest.txt` |
| 六项纯协议CTest | **6/6，4.67秒** | `build/b3b-uart-source-final-protocols.txt` |
| 完整串口门面QTest | **181pass/0fail/0skip，92.128秒** | `build/b3b-uart-source-final-serial.txt` |
| 完整MCP业务QTest | **250pass/0fail/0skip，128.732秒** | `build/b3b-uart-source-final-mcp.txt` |
| 官方SDK2025/rawHTTP2026 | **二十五工具互操作通过** | `build/b3b-uart-source-final-interop.txt` |

原串口取消发布四行均通过，原MCP ACL断言也通过；未删除/放宽断言，不据本轮通过改写历史失败。全部退出码为0（`build/b3b-uart-source-final-results.json`），不合并计作全项目全套。没有运行无关Core/ConPTY、其他平台或真实SSH/UART设备。

```text
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cmake --build build/Release -j 4'
ctest --test-dir build/Release -C Release -R '^novaterm_(history_contracts|capabilities|capability_entry|mcp.*|sftp_session_safety|sftp_panel|command_safety|resource_sharing|session|session_panel|ui_dialog_layout|serial_file_transfer_ui)_tests$' -E '^novaterm_mcp_tests$' --output-on-failure
ctest --test-dir build/Release -C Release -L filetransfer -LE serial --output-on-failure
build/Release/bin/novaterm_serial_file_transfer_tests.exe -o build/b3b-uart-source-final-serial.txt,txt
build/Release/bin/novaterm_mcp_tests.exe -o build/b3b-uart-source-final-mcp.txt,txt
python tests/mcp/interop_check.py
```

Qt bin/plugins沿用6.8.3，QT_QPA_PLATFORM=offscreen、NO_PROXY=*，TEMP/TMP为 `build/b3b-uart-source-final-temp`，SDK依赖仍为build/mcp-test-deps。构建/测试辅助文件与日志不纳入源码。diff空白检查、文档链接检查无新增缺失（`build/b3b-uart-source-doc-check.json`），AGENTS两条HEAD已有历史缺失单列。

已同步MCP入口、00/01/02/03/06、架构索引/路线图、P9与AGENTS；前置子批完成不关闭整个B3b或B4。下一批先冻结独立UART授权及只读发现、原操作取消身份、按波特率有界期限和单文件完成证据，再开放MCP入口。上一轮未提交SFTP修复保留；本轮未提交/推送、未改版本或机械风格。

## 0.2.93 提交收口

用户要求创建本地Git提交并继续开发。按约定将0.2.92→0.2.93，纳入本文件的安全源前置及前轮SFTP两项行为修复。实现源码未在最近完整验证后改变；前节28项CTest、6项纯协议、181项完整串口与250项完整业务仍为各自0.2.92原轮次，不重复运行或改写为新版本成绩。

版本更新后的正常Windows Release默认全部目标构建通过（`build/b3b-uart-source-commit-0.2.93-build.txt`）；本轮协议相关CTest **4/4，11.50秒**，包含contracts/wire/http/endpoint（`build/b3b-uart-source-commit-0.2.93-ctest.txt`）；官方SDK2025/rawHTTP2026二十五工具互操作通过（`build/b3b-uart-source-commit-0.2.93-interop.txt`）。退出码均0（`build/b3b-uart-source-commit-0.2.93-results.json`）。

构建命令沿用前节；CTest筛选改为 `-R '^novaterm_mcp_(contracts|wire|http|endpoint)_tests$'`。TEMP/TMP为 `build/b3b-uart-source-commit-0.2.93-temp`，其余Qt/NO_PROXY前提相同。未运行无关全套、未连接设备，不推送，不纳入build文件。下一子批先落实UART独立发送授权与只读发现目录，随后补发送入口及生命周期。
