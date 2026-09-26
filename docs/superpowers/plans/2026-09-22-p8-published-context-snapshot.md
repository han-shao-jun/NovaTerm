# P8 Published Context Snapshot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 MCP 在持续终端输出期间通过 Parser 按需发布的不可变快照获得稳定上下文，避免 GUI 抢模型锁，并用 A/B 数据决定是否设为默认读取路径。

**Architecture:** `TerminalCore::Runtime` 接收有界、可合并的发布请求，在 Parser 稳定提交点最多 4 Hz 构造 `shared_ptr<const PublishedTerminalState>`；无读取需求时不构造。`TerminalContextProvider` 消费发布对象并保留真实捕获时间，`McpService` 复用同一 session/revision 的基础快照，只在客户端层做 token 和预算投影。新路径先由测试开关启用，性能闸门通过后再成为默认。

**Tech Stack:** C++17、Qt 6.8、std::shared_ptr 原子 free functions、Parser worker、QTest、现有 MCP Python 性能夹具。

**Spec:** `docs/architecture/stages/P8_AI_MCP_Interface.md` §3.2、§3.3、§7.2、§9.1～§9.3、§14。

## Global Constraints

- Parser 单写模型不变；MCP 不得暂停 Parser 等待客户端，也不增加第二个模型写者。
- 无 MCP 客户端或没有读取需求时，不新增周期计时器、后台日志录制或快照复制。
- 同一 session/epoch/revision 的基础快照跨客户端共享；客户端 token/projection 不共享消费进度。
- 发布频率默认最多 4 次/秒；重复请求合并，队列与唤醒保持有界。
- Published snapshot 必须不可变，不跨线程暴露 Core 可变容器、裸指针或借用 string_view。
- `capturedAt` 表示底层快照真实形成时间；复用不得更新时间戳。
- 首次尚无发布物时允许 Busy；已有同 epoch 快照时允许有界陈旧，但必须可观测。
- 保留现有 256 KiB/1024 行硬上限、cacheFloor/resetRevision 和截断语义。
- 使用 `scripts/build-novaterm.bat` 编译；按模块运行 Core、MCP、Session、TerminalSession 测试，不运行全套。
- 性能路径未通过 A/B 前由测试/环境开关控制，不提前写成已默认启用。

## Review Focus

- 发布请求在 Parser 空闲、持续输出和命令队列繁忙三种状态下都必须最终有界处理。
- Session reset/epoch 变化后不得返回旧 Published snapshot。
- 相同 revision 的四客户端不能导致四次 Core 捕获或四份基础文本副本。
- `capturedAt`、snapshotAge 和 revision 必须来自同一不可变发布物。
- 发布构造不能在持锁期间调用 Qt 信号、未知回调或 GUI 对象方法。

---

### Task 1: TerminalCore 按需不可变发布

**Files:**
- Create: `src/core/terminal/PublishedTerminalState.h`
- Modify: `src/core/terminal/TerminalCore.h`
- Modify: `src/core/terminal/TerminalCore.cpp`
- Modify: `tests/core/TerminalCoreTests.cpp`

**Interfaces:**
- Produces: `PublishedTerminalState { TerminalState state; std::chrono::system_clock::time_point capturedAt; }`。
- Produces: `TerminalCore::requestPublishedTerminalState()`，返回当前共享快照并在需要时合并提交发布请求。
- Produces: `TerminalCore::publishedContextStatistics()`，至少包含 request/publish/reuse 计数。

- [ ] **Step 1: 写失败测试固定按需、共享和频率语义**

  覆盖：无请求时 publishCount 不变；首次请求允许空并唤醒 Parser；随后取得非空快照；相同 revision 返回同一 shared_ptr；连续请求只合并为一次发布；写入新文本后下一发布 revision 增长且旧对象保持不变。

- [ ] **Step 2: 使用指定脚本构建并确认 API 不存在而失败**

  Run: `scripts\build-novaterm.bat`

  Expected: Core 测试编译失败，指向 PublishedTerminalState/request API 缺失。

- [ ] **Step 3: 增加可合并 Parser 发布命令**

  为 ParserCommand 增加 `PublishContext`，纳入队尾同类命令合并和现有有界命令预算。GUI 请求使用原子 250 ms gate；只有 gate 放行时入队并唤醒 worker。

- [ ] **Step 4: 在稳定提交点构造不可变快照**

  Parser 持有 modelMutex 时调用现有 `terminalStateLocked(0, MaxBytes, MaxLines)`，构造 shared_ptr 后用 C++17 atomic_load/atomic_store 发布；随后在锁外继续现有信号投递。若 revision 未变化，复用现有对象并只更新统计，不伪造 capturedAt。

- [ ] **Step 5: 运行 Core 精确测试**

  Run: `ctest --test-dir build/Release -C Release -R '^novaterm_core_tests$' --output-on-failure`

  Expected: Core 相关新旧用例全部通过。

### Task 2: Provider 与 MCP 使用真实发布物

**Files:**
- Modify: `src/session/TerminalContextProvider.h`
- Modify: `src/session/TerminalSession.h`
- Modify: `src/mcp/McpService.cpp`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `tests/session/SessionTests.cpp`

**Interfaces:**
- Consumes: Task 1 的 PublishedTerminalState。
- Produces: `TerminalContextProvider::Snapshot::capturedAt` 与共享 `publishedState` 归属。
- Produces: MCP `capturedAt` 基于发布物真实时间，不基于 RPC 响应时间。

- [ ] **Step 1: 写失败测试固定持续写入可读性与 capturedAt**

  持续向 Core 写入有界数据，同时重复读取；发布路径启用后应在预热后持续返回快照而非长期 Busy。复用同一基础快照的两次 RPC 必须返回相同 capturedAt。

- [ ] **Step 2: 运行 MCP/Session 测试确认旧 try-lock 路径失败**

  Run: `ctest --test-dir build/Release -C Release -R '^(novaterm_mcp_tests|novaterm_session_tests)$' --output-on-failure`

  Expected: 新持续写入或 capturedAt 断言失败。

- [ ] **Step 3: Provider 消费发布物并保留增量语义**

  Provider 不再调用 `tryTerminalState/tryModelRevision`；取得发布 state 后只把 `line.id > _historyId` 的 recentOutput 纳入缓存，防止全尾快照重复计数。Session reset 清空 Provider 及发布归属。

- [ ] **Step 4: McpService 切换受控路径**

  增加仅测试/环境可启用的 Published path；启用时不抢 modelMutex，沿用 per-client token/预算。无发布物返回短 retryAfter Busy；有旧发布物则返回真实 capturedAt。

- [ ] **Step 5: 运行 Core、MCP、Session、TerminalSession 测试**

  Run: `ctest --test-dir build/Release -C Release -R '^(novaterm_core_tests|novaterm_mcp_tests|novaterm_session_tests|novaterm_terminal_session_tests)$' --output-on-failure`

  Expected: 四个相关目标通过。

### Task 3: 跨客户端复用、合并与可观测性

**Files:**
- Modify: `src/mcp/McpService.cpp`
- Modify: `src/mcp/McpService.h`
- Modify: `tests/mcp/McpTests.cpp`
- Modify: `tests/mcp/performance_check.py`

**Interfaces:**
- Produces metrics: `coreCaptureCount`、`snapshotPublishCount`、`snapshotReuseCount`、`coalescedReadCount`、`snapshotCopiedBytes`、`projectionCopiedBytes`、`snapshotAgeP50/P95`、`perClientLongestNoSuccessMs`。
- Preserves: 每客户端 captureId、token、授权和 budget 隔离。

- [ ] **Step 1: 写失败测试固定四客户端共享**

  同一 revision 的四客户端并发读取后，Core publish/capture 次数不得随客户端数增长；每客户端 captureId 不同，但基础 Published pointer/revision 相同。撤权或 epoch 改变后任何旧 capture 都不可访问。

- [ ] **Step 2: 增加 per-session capture in-flight 状态**

  每 Session 只允许一个发布请求 pending；后续 read 复用现有快照或计入 coalesced，不重复向 Parser 入队。等待者仍分别执行授权、epoch、token 和预算校验。

- [ ] **Step 3: Capture 改存共享基础引用和投影元数据**

  `Capture` 不再永久保存完整重复基础 JSON；搜索 worker 从 capture 的不可变投影读取。TTL 使用现有 100 ms timer 的最近到期时间主动清理，即使当前没有 jobs 也不无限期保留。

- [ ] **Step 4: 添加指标并更新性能脚本**

  正常负载与过载分组报告每客户端 success/Busy/goodput、最长无成功时间、复制字节和快照年龄；不得把 Busy 延迟混入成功 P95。

- [ ] **Step 5: 运行 MCP 精确测试**

  Run: `ctest --test-dir build/Release -C Release -R '^novaterm_mcp_tests$' --output-on-failure`

  Expected: 授权、capture/search、四客户端共享和指标用例通过。

### Task 4: A/B 性能闸门与文档状态

**Files:**
- Modify: `docs/architecture/stages/P8_AI_MCP_Interface.md`
- Modify: `docs/MCP_Usage.md` only if default path changes
- Modify: `docs/superpowers/plans/2026-09-22-p8-published-context-snapshot.md`

**Interfaces:**
- Consumes: Tasks 1～3 的测试与性能指标。
- Produces: 是否默认启用 Published path 的证据结论。

- [ ] **Step 1: 安装/确认官方 SDK 测试依赖**

  仅当 `build/mcp-test-deps` 缺失时请求网络授权，按 `tests/mcp/requirements.txt` 安装到构建目录，不修改系统 Python。

- [ ] **Step 2: 分别运行正常负载和过载 A/B**

  正常负载要求有效响应率 ≥99%、成功 RPC P95 ≤100 ms、GUI 捕获 P95 ≤2 ms、吞吐下降 ≤5%；过载单独报告 Busy、公平性和最长无成功时间。补采 GUI frame P95，不能用 capture P95 替代。

- [ ] **Step 3: 根据闸门决定默认值**

  全部门槛满足才设为默认；否则保留测试开关并在 §14 记录真实缺口，不把设计目标写成已实现性能结论。

- [ ] **Step 4: 最终相关模块验证**

  Run: `scripts\build-novaterm.bat`

  Run: `ctest --test-dir build/Release -C Release -R '^(novaterm_core_tests|novaterm_mcp_tests|novaterm_session_tests|novaterm_terminal_session_tests)$' --output-on-failure`

  Expected: 指定构建和四个相关目标通过；不运行全套。
