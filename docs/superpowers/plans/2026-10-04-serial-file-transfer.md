# 串口文件传输接线 Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development；用户已要求实现接线，连续完成代码和验证，不重复询问授权。

**Goal:** 将独立 X/Y/Z 协议接入串口终端，提供手动传输入口与进度/取消。
**Architecture:** Session 唯一输入泵分流，仲裁器独占输出，事件驱动控制器及有界文件 worker；UI 单向调用 Session。
**Tech Stack:** C++17、Qt6.8、ElaWidgetTools、独立协议库。
**Spec:** ../../architecture/stages/P9_File_Transfer_Protocols.md 的协议契约和新增串口接线记录。

## Global Constraints

不另开串口、不改接收字节、不让文件二进制进 VT/MCP framer；全部 QWidget 留在 UI。文件 worker 最多一个未完成动作、预算不超过256KiB，线程入口设置 nvterm-file。取消/协议失败断开以保证无残留执行；断线旧任务立即失效。不自动发送设备命令，不默认覆盖文件。

## Review Focus

准备阶段的旧 Core 输出屏障；部分写和迟到 bytesWritten；文件结果与重连代际；输出末帧后普通文本顺序；目标存在/符号链接/原子提交及取消清理。

## Task 1: Controller and File Worker
**Files:** src/session/transfer/SerialFileTransferController.cpp、TransferFileWorker.h/.cpp；已有门面头不可自行改名。
**Interfaces:** SerialTransferRequest/Progress，Controller::Channel（Session 注入），start/acceptBytes/notifyWritable/cancel/abort。
- [ ] 先写控制器测试并看到失败，再实现异步文件准备/按块 I/O/提交、协议驱动、定时器及进度。
- [ ] 首先 reserve，异步准备源/目标，同时等待 coreIdle；coreIdle 成功后 queued GUI 屏障再等待普通待写排空，activate，才发送协议。
- [ ] 输入256KiB上限、高低水位192/64KiB；保留未消费后缀；成功尾随文本先发 visibleRemainder 再解除 active。
- [ ] 输出16KiB上限，按 Channel.pendingWriteBytes 对实际排空确认，不仅按 write 返回推进。
- [ ] worker关闭不阻塞GUI；单任务标识隔离旧结果；UTF8路径安全验证，默认不覆盖、临时文件完成后原子发布。
- [ ] cancel发送CAN并有界清理后disconnectRequired，abort不写旧通道。

## Task 2: Ela UI
**Files:** src/ui/widgets/SerialFileTransferDialog.h/.cpp；TerminalView.h/.cpp；resources/translations/novaterm_{zh_CN,en}.ts。
**Interfaces:** TerminalSession::canTransferFiles()；serialFileTransfer() -> controller（均由根代理接线）。
- [ ] 非模态 ElaDialog，选择协议/方向，上传可多选（X仅1），X接收路径和可选大小，Y/Z目录；开始/取消/关闭与明确进度。
- [ ] 终端右键新增发送/接收/进度入口，只对串口可用；传输准备/活动阻止终端按键、粘贴、VT鼠标上报，但保留本地复制/回看。
- [ ] 提示先在设备启动接收/发送程序后点击开始；无自动命令注入。关闭活动窗口取消。
- [ ] tr()与中英文翻译，保持主题/键盘可访问，进度未知长度用忙碌模式。

## Task 3: Session/Serial/Pump/Arbiter Wiring
**Files:** SessionInputPump、SessionInputArbiter、TerminalSession、SerialTransport；根/tests CMake。
- [ ] 测试准备Lease与MCP互斥、Core旧输出/新输入区别、部分写队列、唯一入站分流与pause原因。
- [ ] 补 bounded serial write/pending/clear；分项Lease reserve/activate/release；Controller由Session按需拥有。
- [ ] attach/reconnect/close/disconnect统一abort；所有直接编译Session的测试目标补充传输组件源和链接。

## Task 4: Integration and Docs
**Files:** tests/session/SerialFileTransferTests.cpp、tests/ui/SerialFileTransferDialogTests.cpp、P6/P9/README/AGENTS/ARCHITECTURE/索引。
- [ ] 实际 Controller + 文件worker + Fake通道，X/Y/Z双向、大小/填充、重复/取消/断线、普通尾随文本、迟到写入。
- [ ] 构建主程序，运行专项、MCP与Session及UI测试；改动跨多个模块，按AGENTS补全套并分列已知失败。
- [ ] 独立审查/重要发现回归修复；实际桌面/真实UART未验收明确记录。当前不自动提交。
