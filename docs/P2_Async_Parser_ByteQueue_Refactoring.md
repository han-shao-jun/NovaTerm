# NovaTerm P2 异步 Parser 与有界 ByteQueue 重构

> 实施日期：2026-07-29  
> 状态：已完成

## 1. 目标与范围

P2 将 libvterm 解析从 UI 线程迁移到专用 Worker，使 Transport 数据到达时
只需进入有界队列；协议解析、ScreenBuffer 写入和终端命令处理均在 Worker
内完成。

本阶段不实现 P3 的增量 GPU 命令缓存，也不实现 P4 的 Chunked Scrollback。

## 2. 数据通路

```text
Transport / UI
    │
    ├─ writeInput(bytes)
    │      ↓
    │  BoundedByteQueue（8 MiB）
    │      ↓
    └─ TerminalCommandQueue
           ↓
       Parser Worker
           ↓
        VTAdapter
           ↓
 ScreenBuffer / ScrollbackBuffer
           ↓
 TerminalSnapshot + 合并后的 Qt 信号
           ↓
       Renderer / UI
```

## 3. 核心设计

### 3.1 有界字节队列

`BoundedByteQueue` 使用固定容量 `QByteArray` 作为环形存储，避免持续输出导致
内存无上限增长。写端在空间不足时等待，形成无损背压；停止时会同时唤醒生产者
和消费者，防止退出死锁。

队列公开以下统计：

- 当前积压与总容量；
- 历史高水位；
- 累计入队/出队字节；
- 生产者等待次数。

### 3.2 Worker 所有权

`VTAdapter` 在 Worker 线程内创建、使用和销毁。UI 线程不直接执行
`vterm_input_write()`，也不访问 libvterm 的可变对象。

Worker 每次最多读取 64 KiB，以减少小包输入造成的 callback、锁和跨线程通知
开销。每批解析结束后再发布合并的 damage、scrollback、title、bell 和 output。

### 3.3 命令串行化

resize、键盘、鼠标、滚轮、焦点、粘贴、默认配色和 scrollback 上限变更都进入
有界命令队列，由 Worker 串行执行。这样可避免 UI 与 Parser 同时修改终端状态。

命令队列最大保存 4096 项；常规 UI 输入不会静默丢失终端输出字节。

### 3.4 快照与线程安全

可写屏幕和 scrollback 模型受互斥锁保护。Renderer 构建一帧时获取一次
值语义 `TerminalSnapshot`，之后只读取该快照，因此活动屏幕在整帧期间稳定，
也避免逐 Cell 加锁。

`waitForIdle()` 使用已提交/已完成的字节和命令计数建立完成屏障，供测试、
benchmark 和需要确定一致状态的调用方使用。

### 3.5 生命周期

销毁 `TerminalCore` 时按以下顺序停止：

1. 标记停止；
2. 停止字节队列并唤醒等待线程；
3. 唤醒命令等待；
4. 等待 Worker 退出；
5. Worker 在线程内销毁 `VTAdapter`。

该顺序避免 use-after-free、遗留线程和条件变量永久等待。

## 4. 变更清单

- `src/core/terminal/BoundedByteQueue.h/.cpp`
- `src/core/terminal/TerminalCore.h/.cpp`
- `src/renderer/TerminalRenderer.cpp`
- `tests/core/TerminalCoreTests.cpp`
- `benchmarks/CoreBenchmark.cpp`
- `CMakeLists.txt`

## 5. 验证结果

| 验证项 | 结果 |
| --- | --- |
| Debug Core 自动化测试 | 10 项通过 |
| Release Core 自动化测试 | 10 项通过 |
| 完整 Debug `NovaTerm` 构建 | 通过 |
| Release Parser 数据量 | 20 MiB |
| Release Parser 耗时 | 2139.78 ms |
| Release Parser 吞吐 | 9.35 MiB/s |
| 10 万行 Scrollback 写入 | 490.63 ms |

新增压力覆盖：

- 环形缓冲回绕后仍保持字节顺序；
- 队列满载时产生可观测背压；
- 256 KiB 小批次异步输入完整消费；
- Parser 工作期间 resize；
- 反复高负载创建、等待和销毁。

## 6. 结论与后续

P2 的线程架构、数据所有权、背压和安全退出目标已经落地，完整应用可编译。
Release 吞吐较 P1 的 4.22 MiB/s 提升到 9.35 MiB/s，但仍低于计划中的
20 MiB/s 性能目标。后续应通过 profiler 定位 ScreenBuffer 同步、scrollback
复制和 libvterm callback 成本；P3 则继续实现 RenderScheduler 与真正的增量
Render Command，避免 Parser 更新与 GPU 帧一一对应。

## 7. 背压补完（2026-07-30）

初版 P2 仅在 `BoundedByteQueue` 满载时阻塞写端。由于 Transport 的
`readyRead` 最终可能在 UI 线程调用 `TerminalCore::writeInput()`，该实现虽然限制了
内存，却可能把 Parser 的压力传回 UI 事件循环，不满足“Parser 阻塞 UI 时间为
0 ms”的目标。

本次补完采用以下端到端流控：

- `TerminalCore::writeInput()` 改为非阻塞入队，并返回数据是否完整接收；
- 8 MiB 字节队列使用 6 MiB 高水位和 4 MiB 低水位，形成带滞回的暂停/恢复；
- `ITransport` 新增 `setReadPaused()`，无法暂停的实现必须返回 `false`；
- Unix PTY 在高水位禁用 `QSocketNotifier`，低水位重新启用；
- Windows ConPTY reader 在高水位等待条件变量，低水位唤醒后继续 `ReadFile`；
- `TerminalView` 暂存竞争窗口内尚未入队的片段，在低水位后按 64 KiB 重试，
  不静默丢失普通终端输出；
- 无法暂停的 Transport 和命令队列满载通过 `inputOverload` 显式报告；
- 命令队列同时受 4096 项和 8 MiB 预算约束，相邻的 resize、默认颜色、
  scrollback 上限和 flush 命令合并为最新值；
- 命令记录入队时的字节完成屏障，避免 resize 等命令越过先前已接收的输出；
- `TerminalCore` 销毁时先停止接收新任务并尝试排空已提交任务，再停止 Worker。

补充测试覆盖：

- 满队列生产者阻塞后由消费者正确唤醒；
- 64 MiB 突发输入不会阻塞调用线程；
- 高水位产生暂停信号，队列排空至低水位后产生恢复信号；
- Release Core 自动化测试共 12 项通过；
- Release `NovaTerm` 完整目标构建通过。

至此 P2 的队列级和 Local PTY/ConPTY Transport 级背压形成闭环。后续
SSH、Serial、Telnet Transport 接入时必须实现 `setReadPaused()`；如果底层协议
不能暂停读取，则必须提供独立接收线程和明确的过载策略。
