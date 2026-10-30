# Direct File Transfers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** AI 对话确认后直接以路径传输，取消目录/串口预授权及重复准备操作。
**Architecture:** 保留应用传输索引和原 Session 执行 owner，以直接路径和原连接快照替代授权根 ID。协议、SFTP、串口三个责任明确的改动分别验证后集成。
**Tech Stack:** Qt 6.8 / C++ / libssh / 原 ZMODEM。
**Spec:** ../specs/2026-10-30-direct-file-transfers-design.md

## Global Constraints

- 不新增执行器、依赖或设备连接；保留固定句柄、64 MiB 文件、64 KiB 分块和原有超时预算。
- 无新确认窗口；按用户追加要求，首次上传返回 confirmation_required，Agent对话确认后带确认凭据及confirmed再次调用；同路径/目标/覆盖意图持久记忆。
- 复用当前检出和本机构建脚本，不提交/推送，不纳入 build 与本机脚本。

## Review Focus

- 本地盘符根/远端根下的单个文件正常处理，拒绝目录冒充文件和相对路径。
- 同 operationId 改路径、方向或覆盖意图拒绝；相同重试不重复传输。
- 等待 SFTP 就绪期间的取消/断开/换绑不启动迟到任务。
- UART 释放原 AI 控制后重新核对连接；其他在途操作不会被抢占。
- 空格/中文路径和省略 remoteName 不产生静默截断或错误文件名。

### Task 1: 协议与路径契约

Files: contracts/FileTransferRequest.h, protocol/ToolCodec.cpp, tests/mcp/ContractsTests.cpp, 协议互操作脚本与相关 HTTP 测试。
Interfaces: StartFileUploadRequest/StartFileDownloadRequest{reference, localPath, remotePath, operationId, overwrite}; StartSerialFileUploadRequest{reference, localPath, remoteName, operationId}。上传 DTO 末尾附 confirmationToken/optional confirmed，确认由Service前置门面处理，不进入传输worker。
- [x] 写 JSON 直接路径受理及旧授权参数拒绝回归，构建 contracts 并记录真实 RED。
- [x] 修改 Schema/解码，两个授权目录工具退役，工具总数 22→20；串口省略 remoteName 默认源名。
- [x] 更新受影响契约/HTTP/互操作断言并验证 GREEN。
- [x] 首次上传两步确认、有界待确认记录、参数/原身份/连接精确绑定、本机路径记忆与清除入口；验证拒绝/伪造/过期/改参均不上传，记忆命中免询问。

### Task 2: SFTP 直接路径与自动准备

Files: FileTransfers.*, files/LocalFileAccess.*, MainWindow.cpp, SftpPanel.*; tests/mcp/FileTransfersTests.cpp, tests/ui/SftpPanelTests.cpp。
Interfaces: 上述直接路径 DTO；原 SftpFileUploadRequest root/name 字段由路径拆分生成，Start/Delivery 接口保持。
- [x] 增补无任何目录授权可上传/下载、精确重试、旧绑定拒绝测试。
- [x] 路径拆分与平台安全 I/O 支持单个根目录文件；authorize只复核统一身份和原连接。
- [x] 原 owner 异步准备/等待 SFTP，并结算取消、失败和销毁；不切换可见面板。
- [x] 运行传输索引、I/O、安全执行及面板回归。

### Task 3: UART 直接路径与控制衔接

Files: SerialFileTransfers.*, SerialFileTargetAdapter.*; tests/mcp/SerialFileTransfersTests.cpp。
Interfaces: 直接路径串口 DTO；原 liveBinding 仍提供真实 I/O generation/config 摘要；原 ControllerResolver不变。
- [x] 增补无文件/target授权发送与默认文件名测试。
- [x] 直接拆分源路径，在原 worker 打开；同主体输入控制释放和原连接复核。
- [x] 保留协议完成/跳过/未知事实、取消代际及资源预算回归并运行。

### Task 4: 界面/文档/集成

Files: McpSettingsDialog.*, FileResourceSharingDialog/RemoteDirectoryDialog（按用户追加要求删除源文件及入口）, docs/MCP_Usage.md、当前契约与AGENTS记录、翻译。
- [x] 去除文件授权入口及不再适用的用户步骤；根据用户答复处理历史授权范围。
- [x] 用户确认历史逐条授权一并取消：统一接入读取已有录制与导入资源，保留身份/资源代际及搜索预算，移除逐条授权控件。
- [x] 正常 Release 默认构建；相关 MCP/录制及面板测试，完整 MCP 文件输出与两代互操作分轮记录。
- [x] qt-cpp-review独立行为复核并修复确认问题，更新结果记录。

## 执行记录

- 基线 4a1333c；用户已选择 AI 对话确认并授权取消类似前置门槛。源码尚未改动时先写协议 RED。

- 全部任务已完成。实际RED/GREEN、夹具迁移、独立复核两项修复与分轮成绩见 [实现记录](../../architecture/Direct_File_Transfers_2026-10-30.md)。未提交或推送，版本保持0.2.105。
- 后续用户要求提交推送：六个独立审查领域未发现高可信新增缺陷，按约定递增0.2.106；新版本独立构建输出及提交前分轮测试详见实现记录末节。默认exe被运行实例占用的链接失败保留，不写成默认构建通过。
