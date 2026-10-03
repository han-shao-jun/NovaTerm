# 文件传输协议 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. 当前用户已要求开始编码，依次验证各任务，不等待重复确认。

**Goal:** 独立实现 XMODEM、YMODEM、ZMODEM 双向文件传输。

**Architecture:** 纯 C++17 事件驱动引擎消费字节和时钟，输出有界字节与异步文件动作。X/Y 共享包格式；Z 独立编解码，公共接口冻结后可分别实施。

**Tech Stack:** C++17、CMake 3.20、标准库测试、lrzsz 0.12.20、ASan/UBSan。

**Spec:** ../../architecture/stages/P9_File_Transfer_Protocols.md

## Global Constraints

不接串口/Session/UI，不链接 Qt，不自建线程，不执行命令。单文件小于 4 GiB，批次最多 256 文件，输入/输出缓存各 64 KiB，动作 payload 最多 256 KiB。中文注释；在用户工作树实施；用户于收尾时要求创建 Git 提交，版本递增至 0.2.55。

## Review Focus

部分写后缀、包号回绕/重复包、控制字节在 payload 内、未知长度尾部保真、取消与旧文件结果均应有独立断言；真实对端验证不能以自研互测代替。

## Task 1: 公共接口、校验与构建

**Files:** src/filetransfer/{TransferTypes,ITransferEngine,EngineSupport,Checksum}.{h,cpp}；CMakeLists.txt；tests/filetransfer/ChecksumTests.cpp。
**Interfaces:** TransferRequest/TransferAction/OperationResult；ITransferEngine 的 start/consume/advance/nextDeadline/pendingOutput/acknowledgeOutput/takeAction/completeOperation/cancel/progress；TimePoint 为单调毫秒。
- [x] 先写 123456789 校验向量和部分写/过期结果测试，观察编译失败。
- [x] 实现纯标准库公共支持，固定上限，独立 CMake 和测试目标。
- [x] 配置并运行共享测试，Expected: PASS，无 Qt 链接。

## Task 2: XMODEM 与 YMODEM

**Files:** src/filetransfer/{XyPacketCodec,XmodemEngine,YmodemEngine}.{h,cpp}；tests/filetransfer/{XmodemTests,YmodemTests}.cpp。
**Interfaces:** 消费 Task 1 的固定接口；提供两个 ITransferEngine 实现与 codec。
- [x] 先编写模式、分片、重复包、长度、回绕、结束与取消测试，观察失败。
- [x] 实现 checksum/CRC/1K XMODEM 和标准 batch YMODEM 双向传输。
- [x] 独立编译并运行两协议测试，Expected: PASS；记录 lrzsz 每种模式互通结果。

## Task 3: ZMODEM

**Files:** src/filetransfer/{ZmodemCodec,ZmodemEngine}.{h,cpp}；tests/filetransfer/ZmodemTests.cpp。
**Interfaces:** 消费 Task 1 的固定接口；提供 ZMODEM Codec 与 ITransferEngine 实现。
- [x] 先写 CRC16/32、ZDLE、任意分片、重复反馈、位置纠错、结束尾随字节测试，观察失败。
- [x] 实现协商、双向文件/batch、有限窗口、ZRPOS、取消和超时。
- [x] 独立编译运行 Z 测试，Expected: PASS；进行 lrzsz 两 CRC 模式双向互通。

## Task 4: 独立对端、质量检查与文档

**Files:** tests/filetransfer/{InteropDriver.cpp,interop_check.py,NoQtLinkCheck.cpp,CMakeLists.txt}；README.md、AGENTS.md、docs/ARCHITECTURE.md、P9/索引/路线图。
**Interfaces:** CLI 驱动 ITransferEngine，通过 raw PTY 和文件宿主连接 lrzsz。
- [x] 双向所有模式与独立对端比较内容，未知长度 XMODEM保留填充。
- [x] 运行纯标准库 no-Qt 链接验收、ASan/UBSan、严格警告、边界与生命周期测试。
- [x] 配置根工程并构建新目标，确认源 GLOB 排除 filetransfer，不编入主程序。
- [x] 独立审查全部变更，修复重要发现；同步文档，注明未验收平台和真实串口。


## 2026-10-04 执行记录

当前协议实现与 Linux 验证完成，详细证据归档于 P9 阶段文档；这是本轮计划的
代码与测试任务记录，不代表 P9 全平台退出标准已经满足。

- 共享基础、X/Y、Z 实现与六项测试已落地；根工程/独立构建均通过。
- 124 个原始 lrzsz 对端用例通过，两个 CRC16 空文件相关用例显式 SKIP。
- ASan/UBSan 六项通过；LeakSanitizer 受 ptrace 限制，GCC 13 fanalyzer
  标准库最小例亦有诊断，两者不记为已通过。
- 独立审查的三个发现均经回归与原复现复验：未排空输出的迟到 ACK、
  过期文件结果、ZCHALLENGE 覆盖重传包。
- 实施决策：保留当前授权工作树，收尾按用户要求提交为 0.2.55；C++17 覆盖技能中的 C++20
  示例；公共支持仅一个在途文件动作（小于设计最多16项），保持落盘顺序。
- Windows/macOS 与串口接线不在本机完成范围内，明确保留验收缺口。
