# 串口文件传输接线 Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development；用户已要求实现接线，连续完成代码和验证，不重复询问授权。

**Goal:** 将独立 X/Y/Z 协议接入串口终端，提供手动传输入口与进度/取消。
**Architecture:** Session 唯一输入泵分流，仲裁器独占输出，事件驱动控制器及有界文件 worker；UI 单向调用 Session。
**Tech Stack:** C++17、Qt6.8、ElaWidgetTools、独立协议库。
**Spec:** ../../architecture/stages/P9_File_Transfer_Protocols.md 的协议契约和新增串口接线记录。

## Global Constraints

不另开串口、不改接收字节、不让文件二进制进 VT/MCP framer；全部 QWidget 留在 UI。文件 worker 最多一个未完成动作、预算不超过256KiB，线程入口设置 nvterm-file。用户/对端取消使用协议 CAN 保留连接；协议失败仍可断开；断线旧任务立即失效。不自动发送设备命令，不默认覆盖文件。

## Review Focus

准备阶段的旧 Core 输出屏障；部分写和迟到 bytesWritten；文件结果与重连代际；输出末帧后普通文本顺序；目标存在/符号链接/原子提交及取消清理。

## Task 1: Controller and File Worker
**Files:** src/session/transfer/SerialFileTransferController.cpp、TransferFileWorker.h/.cpp；已有门面头不可自行改名。
**Interfaces:** SerialTransferRequest/Progress，Controller::Channel（Session 注入），start/acceptBytes/notifyWritable/cancel/abort。
- [x] 先写控制器测试并看到失败，再实现异步文件准备/按块 I/O/提交、协议驱动、定时器及进度。
- [x] 首先 reserve，异步准备源/目标，同时等待 coreIdle；coreIdle 成功后 queued GUI 屏障再等待普通待写排空，activate，才发送协议。
- [x] 输入256KiB上限、高低水位192/64KiB；保留未消费后缀；成功尾随文本先发 visibleRemainder 再解除 active。
- [x] 输出16KiB上限，按 Channel.pendingWriteBytes 对实际排空确认，不仅按 write 返回推进。
- [x] worker关闭不阻塞GUI；单任务标识隔离旧结果；UTF8路径安全验证，默认不覆盖、临时文件完成后原子发布。
- [x] cancel发送CAN并有界收尾后保留连接（用户最新要求），错误清理可请求disconnectRequired；abort不写旧通道。

## Task 2: Ela UI
**Files:** src/ui/widgets/SerialFileTransferDialog.h/.cpp；TerminalView.h/.cpp；resources/translations/novaterm_{zh_CN,en}.ts。
**Interfaces:** TerminalSession::canTransferFiles()；serialFileTransfer() -> controller（均由根代理接线）。
- [x] 非模态 ElaDialog，选择协议/方向，上传可多选（X仅1），X接收路径和可选大小，Y/Z目录；开始/取消/关闭与明确进度。
- [x] 终端右键提供统一“串口文件传输”入口（依当前已合入界面），在窗口选择方向并查看进度；只对串口可用；传输准备/活动阻止终端按键、粘贴、VT鼠标上报，但保留本地复制/回看。
- [x] 提示先在设备启动接收/发送程序后点击开始；无自动命令注入。关闭活动窗口取消。
- [x] tr()与中英文翻译，保持主题/键盘可访问，进度未知长度用忙碌模式。

## Task 3: Session/Serial/Pump/Arbiter Wiring
**Files:** SessionInputPump、SessionInputArbiter、TerminalSession、SerialTransport；根/tests CMake。
- [x] 测试准备Lease与MCP互斥、Core旧输出/新输入区别、部分写队列、唯一入站分流与pause原因。
- [x] 补 bounded serial write/pending/clear；分项Lease reserve/activate/release；Controller由Session按需拥有。
- [x] attach/reconnect/close/disconnect统一abort；所有直接编译Session的测试目标补充传输组件源和链接。

## Task 4: Integration and Docs
**Files:** tests/session/SerialFileTransferTests.cpp、tests/ui/SerialFileTransferDialogTests.cpp、P6/P9/README/AGENTS/ARCHITECTURE/索引。
- [x] 实际 Controller + 文件worker + Fake通道，X/Y/Z双向、大小/填充、重复/取消/断线、普通尾随文本、迟到写入。
- [x] 构建主程序，运行专项、MCP与Session及UI测试；改动跨多个模块，按AGENTS补全套并分列已知失败。
- [x] 独立审查/重要发现回归修复；实际桌面/真实UART未验收明确记录。当前不自动提交。


## 2026-10-07 收尾执行记录

实现已随 ce7793a 合入，后续菜单/窗口调整保留：4788cf3 合并入口，
1b5d5b6 调整宽度。本轮没有重写这些代码。

- 重新构建当前 0.2.66 主程序及相关测试。
- 六项纯协议测试 6/6，Session/门面/UI 专项 3/3 通过。
- 实际 QSerialPort/raw PTY 屏障与尾随文本用例在 Session 目标内通过。
- 当前门面独立 ASan/UBSan 验证 50/50 数据行通过（泄漏检测未启用）。
- 定向审查确认两旧缺陷的修复覆盖：宿主写停滞期限、取消发布回滚与竞争保护。
- P9、P6、总架构、索引/路线图、README 与 AGENTS 已同步接线事实。

勾选表示本轮实施/检查动作已完成，不代表全部平台退出标准满足。
Windows 三个取消发布竞争数据行的已知失败、macOS/真实 UART 文件传输及
桌面/原生选择器人工验收仍是缺口；不把串口文本压力夹具当作文件互通证明。
本轮未新增 Git 提交，版本保持 0.2.66。


## 2026-10-07 用户调整：保留手动检测，协议取消不关闭会话

自动检测未实施，也不新增空闲扫描。取消使用引擎 CAN 序列与有限收尾，
X/Y 正在发送的半包先补必要包尾，收到对端取消后不继续文件载荷；
文件/协议错误与实际断线另行处理。新增门面、窗口取消按钮与真实 QSerialPort
回归；具体运行结果记录在 P9 文档。


取消增量验证：Session/门面/UI 三项 CTest 全部通过，门面独立 ASan/UBSan
90/90 数据行通过（泄漏检测未启用），主程序与翻译构建通过。X/Y 半包与同步
恢复读取回调均记录 RED→GREEN；真实 raw PTY 验证 Session 保持 Running。
