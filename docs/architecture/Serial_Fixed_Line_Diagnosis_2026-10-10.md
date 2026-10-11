# 固定串口行变长的定位记录（2026-10-10）

## 结论与适用范围

**发送时序更正**：早期 std::thread 异步夹具没有 Qt 事件分发器，实际在
末尾集中发送，旧表仅作软件突发证据。最终改为带事件循环的 QThread 后，
旧生产实现仍在 1 秒 GUI 停顿下复现 110 字符长行；修复版双向持续发送、
重新打开和停顿恢复四项均完整。详见末尾的发送核验与最终验收两节。

本机 COM2 → COM1 虚拟串口、115200、8N1、无流控，已复现与用户截图
同类的异常长行。复现中，异常在 `SerialTransport::readyRead` 的原始字节
里已经存在，随后进入 Core，再被 D3D11 正常绘制；不是 GPU 缓存漏清或漏画。

仅改变同一个接收端的驱动 RX 队列，从实际 4096 字节扩至实际 262144 字节，
同负载接收字节恢复完整，异常行消失。证据支持小接收队列在软件突发下截断
数据这一机制。本轮未追踪内核驱动内部溢出事件，不把问题归责为某个 Qt 或
虚拟驱动实现缺陷；用户原始现场未抓包，不能声称逐字节确认了那张截图的来源。

上述定位阶段没有修改生产代码或主程序配置；随后按用户要求实施生产修复，
具体行为、验收与 I/O 隔离评估见本文最后一节，原始定位数据保持不变。

## 原测试的覆盖范围

[2026-10-07 压力报告](Performance_Serial_Stress_2026-10-07.md)的可见场景确实
运行真实 `TerminalRenderer` / D3D11，并记录渲染帧和 CPU 帧耗时；它不是
没有 GPU 的测试。但正确性验收主要核对输入流、Parser 出队、Core 末尾文本，
没有逐帧读取 GPU 画面并逐像素比对。

该报告的无损长测只在测试夹具请求并查询确认 256 KiB 驱动 RX 队列后通过。
定位时 `SerialTransport.cpp` 只设 Qt 软件读缓冲为 256 KiB，没有在生产打开路径
调用 `SetupComm` 扩大驱动 RX 队列。两者不可混同。

旧测试帧为 128 字节，4096 恰好包含 32 帧；固定发送行则是
`123456789` 重复 9 次，共 81 字符，加 CRLF 后为 **83 字节**。
旧报告观察到整帧缺失，不代表其他行长度下不会截断在换行之前。

## 方法与环境

- 源码基线：`12e5ce5`，工作树生产源码未改。
- Qt 6.8.3、MSVC 19.51.36260、Release、真实 D3D11。
- 窗口 1800×1000 逻辑像素；本机截图为 3150×1750 物理像素。
- 复用生产 `SerialTransport → SessionInputPump → InteractiveStreamFramer
  → TerminalCore → TerminalRenderer`，不运行完整 MainWindow。
- 在 `readyRead` 到达时保存原始接收字节，在停止发送后核对活动屏幕和历史。
- 不把一个 `readyRead` 分片当作一行；在整流中查找 CRLF。
- 通过既有 `NOVATERM_SERIAL_STRESS` friend 获取同一端口句柄，以
  `GetCommProperties` 核对扩容前后实际 RX 容量。
- 独立发送线程每次 `write` 83 字节，再请求休眠 1 ms。异步发送模式不在
  每行之后等待写完成，最终排空 Qt 待写队列。这是软件发送实验，Windows
  调度与 Qt 合批会改变实际间隔，不能称为精确 1000 行/秒或真实 UART 线速。
  后续边界核验发现其缺少事件分发器；该段方法和早期表格保留历史记录，
  持续发送与 GUI 停顿的正式结论使用末节更正后的夹具。
- 临时源文件和全部原始结果保存在本机 gitignore 的
  `build/serial-line-diagnostic-src/` 与 `build/serial-line-diagnostic/`。
  这些不是其他机器已有的测试结果，也不注册默认 CTest。

## 分层对照

### 绕开串口

直接输入 Core，分别验证完整行、CR 与 LF 分开到达、遍历行内分片位置、
31 行合批。每个 Parser 批次等待 idle 后检查活动屏幕，结束后检查全部
保留历史。四项均未出现异常行，共输入 48028 行。

D3D11 对应四项共输入 12007 行。停止输入后，渲染版本均与 Core 版本收敛；
隐藏再恢复要求强制全帧重建，重建前后截图逐像素完全相同。实际渲染帧数
分别为 1969、2243、2659、62，截图均无异常长行。

这些结果验证指定软件输入条件与最终画面，没有宣称持续输出每一帧都被
逐像素验证。前两组每批等待 idle，也不用于证明并发吞吐上限。

### 串口发送方式对照

逐行等待写完成的发送方式在 4 KiB 队列下完整接收 12000 行、996000 字节；
另注入一次 120 ms GUI 停顿，在 4 KiB 和 256 KiB 队列下也都完整接收。
这说明仅有小队列不必然丢字节，发送合批和实际排空时序会改变结果。

改为每行异步写入后，以下四项均由同一程序发送 3000 行、249000 字节；
发送端 `write` 全部接受，结束时排空 Qt 待写队列。只有驱动 RX 容量和
注明的人工停顿不同。`write` 接受与本机排空不等于对端收到，完整性以
接收整流与参考整流比对为准。

| 人工 GUI 停顿 | 实际 RX 队列 B | 实收 B | 110 字符长行 | 最终半行字符数 | 整流及 Core |
| --- | ---: | ---: | ---: | ---: | --- |
| 无 | 4096 | 28672 | 6 | 29 | FAIL |
| 无 | 262144 | 249000 | 0 | 0 | PASS |
| 一次 1000 ms | 4096 | 32768 | 7 | 29 | FAIL |
| 一次 1000 ms | 262144 | 249000 | 0 | 0 | PASS |

无人工停顿两项的运行时间分别约 6.565 / 6.493 s；有停顿分别约
6.241 / 6.249 s。这些是包含等待收敛与检查的 case 时间，不是精确发送间隔。
失败项程序返回 28，成功项返回 0；没有把字节丢失掩盖为成功。

## 长行的字节级解释

```text
4096 = 49 × 83 + 29
正常正文长度 = 81
截断留下的前缀 + 后续正文 = 29 + 81 = 110
```

有人工停顿的原始捕获为 8 个 4096 字节块，每一块都等于参考重复行流的
前 4096 字节。整流重组后有 385 条 81 字符行、7 条 110 字符行，以及末尾
29 字符半行。无人工停顿捕获中有 337 条正常行、6 条长行和 29 字符半行。

异常行的前 29 字符为 `12345678912345678912345678912`，紧接着又是完整
81 字符正文。原始捕获中的该行含 CRLF 共 112 字节，Core 快照同样有
110 字符。由此直接排除该复现中的 GPU 绘制残留。

缩放窗口只会重新布局和绘制已经解析的模型，无法找回接收前丢失的 CRLF，
因此这种异常缩放后仍在，符合用户描述。

## 证据与复测

本机结果目录：`build/serial-line-diagnostic/`。

- `core-result.txt`、`gpu-result.txt`：Core 与 GPU 对照。
- `serial-default-async-result.txt`、`serial-large-async-result.txt`：无人工停顿 A/B。
- `serial-default-async-stall-result.txt`、`serial-large-async-stall-result.txt`：人工停顿 A/B。
- `serial-gpu-default-async.bin` / `.png`：无停顿、4 KiB 原始字节和复现截图。
- `serial-gpu-large-async.bin` / `.png`：无停顿、256 KiB 原始字节和正常截图。
- 带 `-stall` 的对应文件保留停顿对照；`mode*-incremental.png` 与
  `mode*-full.png` 保留 GPU 重建前后对照。

显式释放 COM1 / COM2 后，在本机临时程序目录运行：

```powershell
$env:PATH = 'C:\Programs\Qt\6.8.3\msvc2022_64\bin;' + $env:PATH
$env:QT_PLUGIN_PATH = 'C:\Programs\Qt\6.8.3\msvc2022_64\plugins'
.\line_diagnostic.exe --serial --async-tx
.\line_diagnostic.exe --serial --async-tx --large-queue
```

添加 `--stall` 进行 1 秒停顿对照。端口被占用时退出，不关闭其他程序的端口。
Qt 软件待写队列和原始捕获的本轮数据量均有固定上限；不进行无限发送。

本轮还运行 `novaterm_terminal_ops_tests`、`novaterm_renderer_tests`、
`novaterm_renderer_p5_tests`，三项均通过；没有运行无关全套测试。

## 修复方向

首先评估在 Windows `SerialTransport` 成功打开后请求驱动 RX 队列，并查询
实际容量；拒绝或未达到请求值时应有明确诊断。此次扩容仅在临时夹具启用，
跨驱动、真实 UART、重连和拒绝路径仍需单独验收，不能直接当成全面解决。

进一步评估隔离 GUI 绘制与串口 I/O 排空，保留唯一 SessionInputPump 通路、
有界队列与停止语义。复测保留 83 字节固定行及其他非 4096 因数的帧长，
同时核对原始字节、Core 文本与真实 GPU 画面，不能只用 128 字节对齐帧。

## 生产修复与串口 I/O 隔离评估

### 已接入的行为

`SerialTransport` 在 Windows 每次打开并校验线路参数后，通过同一个端口
句柄请求 RX 262144 字节、TX 4096 字节，再独立调用 `GetCommProperties`
读取实际 RX 容量。初始化在 `connected` 信号之前完成。请求和查询各自的
错误码独立保留，只有实际容量至少 262144 才标记为已核验。

请求失败但已有实际容量足够时仍核验通过；容量为 0、查询失败或实际容量
不足均不可当作扩容成功。为兼容不支持容量建议或不报告容量的设备，这些
路径保留连接，在 `connected` 后发出包含请求值、实际值和原生错误码的
非致命警告。`QPointer` 和连接 generation 防止 `connected` 的同步订阅者
删除、断开或重连后，旧诊断继续投递。

非致命警告使用独立的 `ITransport::warningOccurred → TerminalSession
→ TerminalView` 通路，视图以终端方案的 ANSI 黄色显示。它不写 `errorString`、
不改变 Session 状态、不触发文件传输错误处理。首次连接与重连后均可见，
不使用普通串口打开错误的去重规则。没有新增字节入站旁路或改写 CRLF。

`SetupComm` 是容量建议，驱动可采用不同缓冲实现，因此必须读回；
`dwCurrentRxQueue=0` 表示该容量不可用，不能理解为已满足请求或无界。
依据：[Microsoft SetupComm](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setupcomm)、
[Microsoft COMMPROP](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-commprop)。

### 自动化验证

- 初始化回归先在未修复生产路径失败：实际 RX 4096，要求 262144，程序返回 29，
  尚未发送数据。
- `SessionTests` 新增八个请求／查询数据行及重新初始化用例，覆盖扩容、容量
  更大、请求被忽略、容量为 0、请求拒绝、查询失败、两者失败、已有容量足够。
  未初始化空实现时 9 项断言失败，接入后通过。
- Session 警告转发和已连接串口视图显示测试在接线前分别因警告计数为 0、
  终端找不到警告文本而失败；接线后覆盖首次连接、重连、旧附着过滤与非致命
  状态，随正式测试通过。默认测试使用内存传输，不占用真实串口。
- 主程序与全部已有目标构建通过。共用 `ITransport` 信号接口变化触发全套
  回归要求；`ctest -N` 核对 23 项，本轮 Release **23/23 通过**，耗时
  199.71 s，结果为 `build/serial-line-diagnostic/full-ctest.txt`。
- 沙箱内会话测试有三个临时目录写入失败；沙箱外复核通过，最终全套也通过。
  未据此清除 AGENTS.md 中其他机器或历史 Windows 失败记录。

### 发送夹具时序核验与更正

后续扩大到 12000 行／996000 字节时，生产 RX 已确认 262144，旧夹具仍有
一次接收 964882 字节、缺 31118 字节、出现 88 字符长行的失败，保留在
`production-forward-result.txt` 和对应 `.bin` / `.png`，不能计为通过。

增加边界计数后发现旧异步夹具的 std::thread 没有 Qt 事件分发器。
`producer-boundary-result.txt` 记录 `sender_dispatcher=0`、
`queued_before_drain=249000`：3000 行全部滞留在 Qt 待写缓存，循环结束后
才由阻塞等待集中排出。此前“每 1 ms 异步写入”是软件入队频率，不是串口
逐毫秒送达；人工 GUI 停顿也未证明与真实写入重叠。旧 A/B 仍证明接收队列
影响软件突发完整性，但不能据其评价精确持续发送或 GUI 停顿隔离。

最终复测改为独立、显式命名的 QThread，在该线程构造 QSerialPort 并运行
事件循环，1 ms PreciseTimer 逐行提交，`bytesWritten` 累计实际完成字节，
待写为零后才关闭。记录事件分发器、入队结束时积压和最终完成量，避免再次
将软件缓存积压误认为流量已经送达。该更正只在临时夹具，不改变生产线程。

### 最终真实串口与 D3D11 验收（2026-10-11）

共用更正后的发送线程、Core、泵、Renderer、字体和窗口，对照目标只替换
`SerialTransport.cpp` 为 HEAD `12e5ce5` 的旧实现；未重置工作树或覆盖生产源。
旧实现打开后实际 RX 为 4096；一次 1000 ms GUI 停顿下，3000 行已完成写入
249000 字节，实际接收 175242，缺 73758 字节，出现一条 110 字符长行。
该长行在原始字节与 Core 历史中均存在，返回 28。此对照真实发送期间有事件
循环，退出发送时待写只有 83 字节，重现了小队列丢换行这一机制。

修复版以下四项没有由夹具调用 SetupComm 扩容，均先核验生产打开路径已经
提供 262144 字节 RX。发送线程的事件分发器、稳定计时策略和实际写完成计数
均有效；退出发送时待写为 83 字节，随后排空，再关闭发送端。原始整流与
参考整流逐字节一致，所有保留历史及活动屏幕的非空行都是 81 字符。

| 方向与打开方式 | 人工 GUI 停顿 | 行数 | 入队／实际写完成／接收 B | 异常行 | 结果 |
| --- | --- | ---: | ---: | ---: | --- |
| COM2 → COM1，首次打开 | 无 | 12000 | 996000 / 996000 / 996000 | 0 | PASS |
| COM1 → COM2，关闭再打开 | 无 | 12000 | 996000 / 996000 / 996000 | 0 | PASS |
| COM2 → COM1，首次打开 | 一次 1000 ms | 3000 | 249000 / 249000 / 249000 | 0 | PASS |
| COM1 → COM2，关闭再打开 | 一次 1000 ms | 3000 | 249000 / 249000 / 249000 | 0 | PASS |

四项持续运行真实 D3D11 并保留截图，但不是逐帧像素验收。累计接收
2490000 字节，无异常行，程序均返回 0；运行时间分别为 12.852、13.045、
3.833、3.864 s，包含收敛与校验，不用它们推断精确毫秒级线速。

结果为 `baseline-qt-control-result.txt` 及 `final-*-result.txt`，对应原始字节
和截图为 `baseline-serial-...-qt-events.*` / `production-serial-...-qt-events.*`。
临时夹具的反向参数使用 `--reverse-ports`，避免 QApplication 将通用
`--reverse` 参数当作 UI 方向选项消费；早期 `corrected-reverse-*` 记录实际
仍是正向，不计为反向通过。另一次尝试在线请求缩回 4 KiB 时，实际容量未
降到 4 KiB，预检返回 34，不发送数据、不计为有效失效对照。

显式释放两个虚拟串口后，在临时程序目录复测：

```powershell
.\line_diagnostic_before.exe --serial --async-tx --qt-event-sender --baseline-control --stall
.\line_diagnostic.exe --serial --async-tx --qt-event-sender --require-production-queue --long
.\line_diagnostic.exe --serial --async-tx --qt-event-sender --require-production-queue --reverse-ports --reopen --long
.\line_diagnostic.exe --serial --async-tx --qt-event-sender --require-production-queue --stall
.\line_diagnostic.exe --serial --async-tx --qt-event-sender --require-production-queue --reverse-ports --reopen --stall
```

不把早期大批突发失败清除为通过。256 KiB 并非任意突发或无限 GUI 停顿的
无损保证；本轮修复验收限于上述虚拟串口、固定行、真实事件循环与指定停顿。

### I/O 与 GUI 隔离的评估结论（迁移前）

独立串口 I/O worker 可隔离 GUI 渲染、布局和模态操作的停顿，减少依赖驱动
缓冲吸收突发的程度。但现有 `_port` 是无 QObject parent 的值成员，直接对
SerialTransport 调用 `moveToThread` 不会自动迁移它；Qt 官方明确指出
QObject 成员变量不会自动成为其 child，除非显式设置 parent。
参见 [Qt QObject 线程归属](https://doc.qt.io/qt-6.8/qobject.html#thread-affinity)。

安全迁移需要同时完成以下约定，不能只改一条线程连接：

1. 在 `nvterm-serial` I/O worker 入口命名线程并在该线程构造、打开、关闭
   QSerialPort；GUI 侧保留 ITransport 门面，所有公开信号仍在 GUI 发出。
2. worker → GUI 收件箱有明确字节上限、统计、暂停与停止语义；最多保留一个
   queued delivery，分块交付到现有 SessionInputPump，不能无限堆积 QByteArray
   排队事件。停止、换绑和重连时旧 generation 的字节与诊断全部失效。
3. 普通写入与文件传输共用有界出件箱。`tryWriteBounded` 的同步接受字节数、
   `pendingWriteBytes` 的计数和 `bytesWritten` 的底层完成事件要有一致定义；
   不得把“交给 worker”误报为“驱动已写完”。
4. 文件传输取消依赖 coreIdle、待写排空、当前 X/Y 包尾和 CAN 的先后顺序。
   `clearPendingOutput` 需同时失效尚未提交给驱动的 worker 字节并建立确认
   屏障，不能让已取消的普通／协议字节在之后复活，也不能阻塞 GUI 等待 worker。
5. 首次打开失败、重复关闭、暂停后恢复、重连、端口被占用、关闭时输出、协议
   取消及反向压力需要跨平台复验，尤其 Windows 和真实 UART。

上述驱动队列修复阶段保留原串口线程模型。
更大的缓冲仍不能保证任意长 GUI 停顿或无限软件突发无损；线程隔离也不能
替代设备流控、物理 FIFO 与线路误码验收。随后按用户要求实施以下线程隔离。

## 串口 I/O 与 GUI 解耦实施及验收（2026-10-11）

### 实现约定

`SerialTransport` 仍为 GUI 的 ITransport 门面，QSerialPort 在独立
`nvterm-serial` 事件循环中构造、配置、打开及析构。线程仅处理字节和
连接，不持有 Core、Session 或 UI 指针。GUI 每次最多交付 4×64 KiB，
所有对外信号仍在 GUI 线程，唯一入站保留为 SessionInputPump。

`SerialIoState` 用同一互斥锁保护 8 MiB 收件箱与 1 MiB 写侧（包括在途
字节），小数据合并成最多 64 KiB 分段，避免逐字节节点与前缀移动。
通知与控制唤醒各有闩锁，最多保留一个待处理事件；generation 失效旧
连接，writeEpoch 失效清写前的完成通知。GUI 暂停交付时端口继续排空，
满箱后保留已有数据、停止排空并警告。关闭和析构无需 GUI 阻塞等待。

底层一次只提交最多 1 KiB。同步接受表示已进入有界出件箱，底层
`bytesWritten` 才表示实际完成。`pendingWriteBytes()` 从同一快照判断
连接及总量；后端已关闭但 GUI 尚未发布关闭时返回 -1，不能将丢弃误报
为排空成功。

清写丢弃尚未提交字节，保留已提交的有限前缀直到排空，确认屏障后才提交
新写。Windows Qt 6.8.3 的 `clear()` 会清软件 writeBuffer，但在途
writeChunk 要等待完成；用 PurgeComm 中止 WriteFile 还可能收到
ResourceError，因此不以它冒充清写成功。取消的串口排空预算包含有限
前缀；确认超时先断开，再释放文件传输独占。没有新增终端输入旁路。

### 实际端口与真实渲染

使用 COM1↔COM2 虚拟串口、8N1、无流控。固定行发送者有独立 Qt 事件
循环及实际写完成计数；高速夹具的独立发送线程每个分块都通过
`waitForBytesWritten()` 排空后再推进完成计数，没有异步缓存末尾集中
排出的时序问题。接收端运行生产 Transport、SessionInputPump、Core 和
真实 D3D11 TerminalRenderer；GUI 停顿为主线程 Sleep，期间渲染与交付
确实停止，但独立 I/O 收件计数必须增长。

| Case | 实际 RX B | GUI 停顿 ms | 完成／接收 B | 停顿期间 I/O 收件 B | 结果 |
| --- | ---: | ---: | ---: | ---: | --- |
| COM2→COM1，115200，83 B 固定行×12000 | 4096 | 5000 | 996000 / 996000 | 403961 | PASS，异常行 0 |
| COM2→COM1，8M 定速，128 B 帧×65536 | 262144 | 5000 | 8388608 / 8388608 | 4001024 | PASS，丢帧／损坏 0 |
| COM1→COM2，8M 定速，128 B 帧×65536 | 4096 | 5000 | 8388608 / 8388608 | 4000384 | PASS，丢帧／损坏 0 |

两项 8M 定速实际约 792.8 KB/s；每项流 SHA-256 一致，泵接受与 Core
出队都是 8388608 B，末尾 512 帧模型文本完整，各有 331 个真实渲染帧。
停顿期间 GUI 交付计数不变；收件箱峰值分别 4001024 / 4000384 B，
满箱和关闭丢弃计数均 0。不是逐帧截图像素验收或真实 8M UART 证明。
4 KiB case 为显式实验覆盖，生产默认仍请求并核验 256 KiB。

另用真实端口验证：GUI 暂停 500 ms 时 1 MiB 出站实际完成，接收内容
一致；清写后新标记位于末尾，旧出件箱没有复活；门面销毁后 GUI 暂停
200 ms，I/O worker 数已回到 0。日志为 `io-lifecycle-result.txt`，返回 0。

可复现的正式压力入口位于 `tests/benchmarks/serial-stress/`，以下从已
构建的夹具目录执行（Qt bin 与插件路径按 AGENTS.md 设置，先释放端口）：

```powershell
.\serial_stress.exe COM2 COM1 8000000 20 visible paced io-highrate-5s.json --bytes 8388608 --stable-timers --deferred-verify --tx-chunk 1024 --gui-stall-ms 5000
.\serial_stress.exe COM1 COM2 8000000 20 visible paced io-reverse-small-5s.json --bytes 8388608 --stable-timers --deferred-verify --tx-chunk 1024 --gui-stall-ms 5000 --driver-rx-queue 4096
```

结果保存在 `build/serial-line-diagnostic/` 的同名 JSON / txt；固定行
使用临时 `line_diagnostic.exe --serial --async-tx --qt-event-sender --long --long-stall --force-small-queue`。
汇总见 [解耦验收 CSV](serial-io-decoupling-2026-10-11-cases.csv)。

### 回归及边界

状态、失败打开、即时销毁、回调内重连、暂停恢复与清写均有确定性单测。
回调内重连丢通知、异步清写取消期限不足、屏障超时释放三个问题都先以
失败回归复现再修复；独立复核又纠正关闭状态查询的两次加锁竞态。
GUI 信号归属与 worker 回收也由 Session 测试检查。

最终主工程及正式压力夹具构建通过，校验器自检通过；Windows Release
CTest **23/23，0 失败，207.84 s**，含串口文件传输取消与终端联通。
完整日志为 `build/serial-line-diagnostic/decoupled-full-ctest.txt`。
此次单机结果不清除其他机器或历史已知失败记录，改动未提交。

随后 Qt C++ 全项审查追加修复两处问题：D-001 的 CAN 已排空但静默
等待达到总期限时，直接结束取消，不新建空清写屏障；真实交接状态的
异步回归先复现 1 次意外断连，修复及相关取消检查为 10 PASS / 0 FAIL。
D-002 的即时析构检查改为逐个等待本轮交接状态弱引用过期，覆盖仍待
启动的线程及其对象回收。临时禁用线程自回收的对照中，旧测试通过而
新测试失败；对照后已恢复生产回收连接，不只凭瞬时活动计数判定完成。
追加修复后 Session、串口文件传输与传输 UI 三项 CTest 为 3/3、0 失败，
110.19 s，日志 `build/serial-line-diagnostic/review-fixes-ctest.txt`；此次
按改动范围复验，不将此前 23/23 写成追加修复后的新全套结果。运行中的
主程序锁住原输出，已用同一对象及库链接 `NovaTerm-review-fixes.exe`，
未中断当前会话。

提交收尾版本为 0.2.67。用户明确要求跳过提交前新回归，沿用上述已完成
验证；新增构建在 `novaterm-mcp.exe` 被占用的链接步骤以 LNK1104 停止，
不宣称 0.2.67 全量构建或新一轮全套回归通过。

独立 I/O 已解除 GUI 停顿对端口排空的直接阻塞，但 8 MiB 仍是有界缓冲。
8M 的理论 800 KB/s 下约可吸收 10.5 秒纯积压，实际余量随已有积压变化；
持续生产快于消费、无限 GUI 停顿或驱动／系统调度停顿仍需设备流控。
真实 UART、其他驱动、Linux/macOS 的此次线程改动尚未实测，不据本机
虚拟串口通过宣称所有工况无损。此前失败和夹具更正记录继续保留。
