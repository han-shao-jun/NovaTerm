# 串口 I/O 与 GUI 解耦实施计划

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** GUI 停顿不再阻止串口端口的读取、写入和关闭；保留字节顺序、背压、取消与重连语义。

**Architecture:** SerialTransport 继续在 GUI 线程实现 ITransport。单个独立 worker 在自己的事件循环中构造、打开和关闭 QSerialPort，跨线程通过有界收发状态与合并通知交接。

**Tech Stack:** Qt 6.8、C++17、QThread、QSerialPort、互斥保护的有界队列。

**Spec:** `docs/architecture/Serial_Fixed_Line_Diagnosis_2026-10-10.md` 的隔离评估及用户要求。

## Global Constraints

- 唯一入站为 ITransport::readyRead → SessionInputPump → Core；不新增 Parser 输入旁路。
- ITransport 信号在 GUI 线程发出；worker 不持有 UI、Core 或 Session。
- 接收队列 8 MiB，Qt 软件读缓冲 256 KiB，写侧含待提交和 Qt 待写共 1 MiB。
- 所有队列有统计、停止语义和 generation；最多一个待处理收件通知与控制唤醒。
- 每个线程入口首先设置 `nvterm-serial` OS 线程名，Qt objectName 共用常量。
- 保留 Windows 驱动 RX 初始化和实际容量核验。
- 原实施阶段不提交、不改版本号；后续用户明确要求创建 Git 提交，
  按仓库约定递增版本为 0.2.67，继续使用当前工作树。

## Review Focus

- GUI 长停顿与 Parser 背压：I/O 继续读到有界收件箱，满后保留数据并暂停排空，不丢弃前缀。
- 重连、关闭和迟到完成：旧 generation 的字节、诊断和写完成失效。
- 部分写与排空：pendingWriteBytes 包含全部尚未完成的字节，bytesWritten 只报告底层完成。
- 取消清写：新写入排在清空确认之后，待写不能提前显示为零。
- worker 生命周期：端口只在 I/O 线程访问和析构，关闭不依赖 GUI 往返。

## Task 1: 有界交接状态与取消屏障

**Files:** `src/transport/SerialIoState.h`, `tests/session/SessionTests.cpp`。

- [x] 新增接收上限、暂停交付、合并通知、停止清空与 generation 测试，先确认失败。
- [x] 新增写侧有界部分接受、底层完成记账、清写屏障与新旧 epoch 测试，先确认失败。
- [x] 实现线程安全状态；只由 worker 接触 QSerialPort。
- [x] 相关 Session 单测通过（首次实现 64 PASS / 4 平台 SKIP；审查修复后续复验）。

## Task 2: 独立端口 worker 与 GUI 门面

**Files:** `src/transport/SerialTransport.h/.cpp`、必要的 Session 取消屏障接线。

- [x] 把端口从 GUI 值成员改为 worker 内构造的资源；保留公开接口并明确异步清写契约。
- [x] 有界分块收发、单次 GUI 交付、状态诊断与写完成合并、重连／关闭失效。
- [x] 验证失败打开、关闭期间重连、暂停恢复、清空期间新写与 worker 回收。
- [x] 编译主程序及相关测试目标。

## Task 3: 真实接收与协议回归

**Files:** `tests/benchmarks/serial-stress/main.cpp`、夹具说明及架构／P6 文档。

- [x] 夹具改为读取线程安全诊断，不跨线程访问原生端口；保留实验覆盖。
- [x] 有事件循环的独立发送端、真实 D3D11、GUI 停顿期间 I/O 收件箱计数增长。
- [x] 验证 4 KiB 与生产驱动队列、双向持续输入、较高字节率、重连／关闭与清写。
- [x] 串口文件传输／取消相关测试及共用接口变化后的全套回归；已知失败如实记录。
- [x] 独立代码审查、修复确认问题，更新文档和所有验收边界。

## 执行记录

- 初始状态：前序驱动队列修复、独立非致命警告与单测已在当前工作树；23/23 CTest 曾通过。保持这些改动，不重做或丢弃。
- Ruling: 本轮延续当前已授权工作树，不另建工作树，也不要求重复批准上述隔离方案。
- Ruling: 保证有界接收与可观测过载，不能承诺无流控设备在无限 GUI 停顿或无限突发下物理无损。
- Ruling: 清写丢弃未提交出件箱，已提交底层的最多 1 KiB 前缀排空后确认屏障；避免 Windows Qt 6.8 PurgeComm 中止在途 WriteFile 导致 ResourceError。新数据不得越过屏障，取消预算包含该前缀，屏障超时则断开后释放独占。
- 审查修复：回调内重连通知闩锁回归已观测失败（1 次收件 vs 2 次预期）；异步清写取消期限与超时释放回归分别观测 CAN 缺失和未断开，修复后的 9 项取消检查通过。
- 审查修复：后端关闭先于 GUI 发布的间隙回归已观测 pending=0 而预期 -1；修正后又将连接状态与总量统一从一次快照读取，消除两次加锁之间的关闭竞态。独立复核未发现进一步明确阻塞问题。
- 真实验收：RX 4 KiB、83 B 固定行×12000、GUI 停顿 5 秒接收 996000 B 完整；双向 8M 定速各 8 MiB、D3D11、GUI 停顿 5 秒完整，反向 RX 4 KiB。停顿期间 I/O 各收约 4 MB、GUI 交付停止，帧／SHA-256／模型均通过。
- 生命周期：真实端口 GUI 停顿 500 ms 仍完成 1 MiB 出站；清写后新标记居尾；门面销毁后 GUI 停顿 200 ms，worker 回到 0。此项不替代真实 UART／跨平台验收。
- 最终验证：主工程和正式压力夹具构建通过，校验器自检通过；Windows Release CTest 23/23、0 失败，207.84 s。结果在 `build/serial-line-diagnostic/decoupled-full-ctest.txt`；不据此删除其他机器／历史已知失败记录。`git diff --check` 通过。
- 用户调用 Qt C++ 全项审查后追加修复 D-001：真实 SerialIoState 异步清写回归先出现 1 次意外断连，已排空输出的取消期限分支现在直接结束、不新建空清写屏障。相关取消检查 10 PASS / 0 FAIL。
- 追加补强 D-002：测试保存每次创建的交接状态弱引用，逐个等待过期，涵盖门面、worker 与线程对象释放。临时移除 QThread 自回收连接时，旧活动计数测试仍通过，新判据观测失败；回收连接已恢复，不新增生产测试 API。
- 两项审查修复后的相关 CTest 为 Session／串口文件传输／传输 UI 3/3、0 失败，110.19 s，日志 `build/serial-line-diagnostic/review-fixes-ctest.txt`。主程序原输出被正在运行的 NovaTerm 锁定（LNK1104），未中断用户会话；同一主目标对象与库已成功链接为 `build/Release/bin/NovaTerm-review-fixes.exe`。源码及文档保留未提交。
- 提交收尾：用户要求创建本地 Git 提交，版本按 AGENTS.md 从 0.2.66 递增为 0.2.67；随后明确要求跳过新回归，故不复跑 CTest。已启动的构建因 `bin/novaterm-mcp.exe` 输出被占用而在链接阶段以 LNK1104 停止，不计为 0.2.67 构建通过，不中断正在运行的程序。
