# 2026-09-10 性能优化与验收记录

基线代码：本地 master `6ced4a5`。按 effective-cpp 与 cpp-coding-standards
实施，保持 C++17、Parser 单写、Session 运行期边界和 QRhi 线程归属。
按用户要求，同批纳入已有 `PtySession.h/.cpp` 改动：`drainOutput()` 的读缓冲
不做多余零初始化，并将启动流程注释校正为 openpty + fork + execve。
Agent 上下文作为 Session 内置只读接口提供。

## 修改文件、函数与原因

| 文件 | 函数/类型 | 原因与实现 |
| --- | --- | --- |
| `src/transport/SshTransport.h/.cpp` | `workerMain`、控制入口 | 固定 20 ms 轮询不能即时响应控制；改为 wakeup fd + 网络事件 + deadline。所有 session/channel libssh 调用仍在 SSH worker |
| `src/transport/SshWorkerWakeup.h` | `notify`、`consume` | RAII 管理可靠 socket 对，最多保留一个通知；Unix socketpair / Windows 本地 TCP |
| `src/transport/SshTransport.h/.cpp` | `emitReadyRead`、`deliverInbound`、`inboundStatistics` | 每次读取投递 Qt 事件会积压；1 MiB 缓冲、64 KiB 分批、单一调度标志，暂停恢复及世代失效，远端 EOF 延后发布 disconnected |
| `src/session/SessionInputPump.h/.cpp` | `acceptBytes`、`drainPending`、`statistics` | pending 前删改 head，按剩余未消费字节限容并统计，排空清理；SSH shellPendingWrite 同样改 head |
| `src/renderer/TerminalRenderer.h/.cpp` | `makeInstance`、`uploadCommands` | 消除 resize→append→takeLast→resize，直接构造并写入目标槽 |
| `src/renderer/glyph/GlyphRasterizer.h/.cpp` | `AsyncGlyphRasterizer`、`rasterize` | 原队列入队后同步取出；改为专用线程、512 个请求/去重标记、32 个结果、每位图 256 KiB 上限，条件变量阻塞、generation 取消及 stop/join |
| `src/renderer/TerminalRenderer.h/.cpp` | `ensureGlyph`、`render`、`rebuildCommandRow` | 渲染线程消费位图、插入 atlas；只重建仍缺字形的行，标记随 row-slot 滚动旋转；worker 不访问 QRhi |
| `src/transport/SshTransport.cpp` | shell/command/monitor read loops | 主流单轮 256 KiB/2 ms，辅助每 stream 64 KiB/2 ms；缓冲残留时下一轮不阻塞，避免交互饥饿 |
| `src/renderer/HistoryLayout.h`、`TerminalRenderer.h` | `remove`、逻辑索引 | 历史布局头删改逻辑 head，至少 4096 个且占一半才 compact；core scrollback 本已用 deque::pop_front，无重复改造 |
| `src/core/terminal/TerminalState.h`、`TerminalCore.h/.cpp` | `terminalState`、`commitPendingModelRevision` | 一次模型锁读取 revision/cursor/title/模式与有界 UTF-8；标题独立变化也推进 revision；控制字符过滤，软换行跨接缝拼接，超长前缀限额 |
| `src/core/terminal/VTAdapter.h/.cpp` | `onSetTermProperty`、`alternateScreen` | 在 libvterm 边界内记录 alternate-screen 状态，仅导出 bool |
| `src/core/terminal/ScrollbackBuffer.h` | `logicalLineAt` | 在已有模型锁内借用逻辑行，避免深拷贝长历史或封存完整快照 |
| `src/session/TerminalStateFilter.h` | `meaningful` | 忽略空文本及独立 ASCII/Braille spinner；CR 覆盖由解析器完成，活动行不进入追加缓存 |
| `src/session/TerminalStateCache.h` | `append` | 256 KiB/1024 行有界缓存，窗口内相同文本去重，淘汰维护 revision floor |
| `src/session/TerminalContextProvider.h` | `context`、`reset` | 同 revision 不重复采样；提供 sinceRevision、viewport、recentOutput、maxBytes/maxLines，alternate 仅 viewport；切换/截断/过期显式提示 reset |
| `src/session/TerminalSession.h/.cpp` | `terminalContext`、`resetForReuse` | 按需创建内置 Provider，Session 复用时清理；返回独立值对象 |

测试文件：`tests/transport/SshTransportFailureCheck.cpp`、
`tests/session/SessionTests.cpp`、`tests/renderer/RendererP5Tests.cpp`、
`tests/benchmarks/RendererP5GpuBenchmark.cpp`。

## 实测环境

Linux 6.8.0-139-generic / Ubuntu 24.04，Intel i7-14700K，Release / GCC 13，
Qt 6.8.3，NVIDIA RTX 4070 Ti、595.84 驱动，OpenGL、约 60 Hz、DPR 1.82292。
以下为单次短时运行值，非改动前后 A/B 比较，不外推广域网或其他 GPU 后端。

## 自动化与负载结果

- 全工程构建通过；当前 Linux 配置注册的 9 项 ctest 全通过。
- 新增 SSH 提前通知、合并通知、阻塞唤醒，10/50/100 MiB 本地 inbound
  逐字节完整性、暂停恢复、EOF 顺序和关闭后旧事件失效检查通过。
- Agent 测试覆盖 100 次 CR progress、重复完成行、ASCII spinner、100 次
  alternate-screen top 风格刷新、标题、跨历史接缝软换行、UTF-8 字节截断与
  缓存淘汰；输入泵 10 MiB 突发及后缀完整性通过。
- glyph worker 测试覆盖 128 种 CJK/组合字符、去重、非调用线程完成、旧
  generation 拒绝与 stop；历史布局 10 万行饱和后再淘汰/追加 10 万次通过。

隔离本机 SSH（临时密钥与 known_hosts、127.0.0.1:42222）：

| 场景 | 最新结果 |
| --- | --- |
| idle | 2.102 秒内进程 CPU 0.014% |
| 10 MiB | 231 ms，负载逐字节核对通过 |
| 50 MiB | 1163 ms，负载逐字节核对通过 |
| 100 MiB | 2274 ms，负载逐字节核对通过 |
| 输出中的 auxiliary command / monitor | 两者均完成 |
| Ctrl+C | 7 ms（Transport::write → Shell 完成标记） |
| resize | 31 行 × 91 列验证通过 |
| Ninja | 隔离目录 32 个真实 C++ 编译目标通过 |

Core 基准含 ANSI/CJK，120×40，64 KiB 输入块、10 万行历史：

| 输入（实际批次取整） | 时间 | 吞吐 | 入队/出队字节 |
| --- | --- | --- | --- |
| 10.06 MiB | 406.81 ms | 24.73 MiB/s | 10548720 / 10548720 |
| 50.05 MiB | 1818.10 ms | 27.53 MiB/s | 52481520 / 52481520 |
| 100.04 MiB | 3532.51 ms | 28.32 MiB/s | 104897520 / 104897520 |

三项均达到现有 20 MiB/s 判据，队列峰值不超过 8 MiB、最终为零。
10 万行 ingestion 约 63.4–64.4 万行/秒。

QRhi OpenGL，预填 10 万行，滚动 3.116 秒：60.013 FPS，重建行 P95=1，
CPU 帧 P50/P95/P99=0.347/0.839/2.991 ms，glyph 队列深度最终 0，
published/rendered revision=240/240，`acceptance=pass`。
暖 CJK/Unicode 无新 raster/atlas upload，overlay 无 content upload。
冷 glyph 初次为空，后续局部帧补齐；一次冷字符因此可出现两次行重建。

首轮 GPU 验收曾提前在字体重建帧结束，队列尚未排空；随后明确等待异步
完成，并加强 forced-full 实际重建行数判据。期间也把 glyph 完成触发的
全屏重建收窄为 pending 行，未降低现有验收阈值。

## 复现命令

```bash
cmake --build build/Release -j 2
ctest --test-dir build/Release -C Release --output-on-failure
build/Release/bin/novaterm_ssh_transport_check --local-ssh-check
build/Release/bin/novaterm_core_benchmark --bytes 10485760 --lines 100000
build/Release/bin/novaterm_core_benchmark --bytes 52428800 --lines 100000
build/Release/bin/novaterm_core_benchmark --bytes 104857600 --lines 100000
build/Release/bin/novaterm_renderer_p5_gpu_benchmark --duration-ms 3000 --scrollback-limit 100000 --prefill-lines 100000 --allow-occlusion
```

## 剩余风险与适用边界

- 未测 Windows/macOS、D3D/Vulkan/Metal、真实多屏或 30 分钟长稳；Windows
  wakeup socket 实现尚未在该平台编译/实跑。
- SSH 为本机网络，未测试用户真实服务器、高延迟/断网与各种认证超时；连接与
  认证阶段仍沿用阻塞 libssh API，控制 wakeup 优化针对已建立会话的 event loop。
- Ctrl+C 数字是 Transport 路径，未包含 GUI 键盘事件经过 Parser 排序屏障的
  全链路延迟；Ninja 为 32 目标夹具，没有执行完整 Linux kernel build。
- Agent 是有界、按需采样摘要，不是无损审计日志；两次查询间超出预算会标记
  truncated/resetRequired。重复去重仅针对缓存窗口内完全相同文本，不归并
  不同时间戳/数字的日志。活动行保留在 viewport，只有完成行追加增量。
- 未实现外部 Agent 网络调用；Session 线程调用 Provider，拿返回
  值跨线程使用。原始终端内容仍可能含敏感文本，访问控制由接入方承担。
- 异步位图失败在当前 generation 负缓存，避免无限重试；字体后端内存不包含
  在位图队列预算内。GPU 图片的人工逐像素视觉验收尚未完成。
