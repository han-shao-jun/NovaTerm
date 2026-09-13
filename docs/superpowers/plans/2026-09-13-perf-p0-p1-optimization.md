# NovaTerm P0/P1 性能热点优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 降低持续输出时主线程 Renderer 数据准备成本和 Parser moverect Cell 同步成本，并移除可确认的空闲 UI 轮询。

**Architecture:** 保持 Parser 单写、Renderer 只读、QRhi 资源仅渲染线程访问。先消除同一行 Cell 的重复哈希与临时容器工作，再收窄 GPU staging 数据准备；Parser 侧只在 VTAdapter/libvterm 边界增加批量读取能力，不改变 moverect 读取最终目标区域的正确性语义。

**Tech Stack:** C++17、Qt 6.8 Widgets/QRhi、libvterm、CMake、Qt Test、perf。

**Spec:** `docs/ARCHITECTURE.md`、`docs/architecture/Rendering_Architecture.md`、`docs/architecture/stages/P1_ScreenBuffer_and_VTAdapter.md`、`docs/architecture/stages/P5_Glyph_and_GPU_Pipeline.md`，以及用户提供的 `/home/super/perf.data`。

## Global Constraints

- Parser 单写，Renderer/Search 只读；跨线程只传不可变快照或版本化数据。
- libvterm 类型只能存在于 VTAdapter 实现边界。
- 不恢复本地旧 Cell 的 moverect 复制；必须读取 libvterm 当前目标区域。
- QRhi、GlyphAtlas 和 GPU buffer 只在 Renderer 线程访问。
- 现有8 MiB ByteQueue、6/4 MiB背压水位和64 KiB Parser批次保持不变。
- 每项先建立能捕获错误行为的测试，再做最小实现；性能收益用相同 workload A/B。
- 当前 `novaterm_ui_dialog_layout_tests` 的滚动范围失败单独记录，不计为本轮回归。

---
> **执行状态（2026-09-13 更新）**：Task 1–7 已全部实施并验证，35 个步骤全部完成；
> Task 6 Step 4 的窗口人工验收已由用户完成。逐项证据、A/B 数据与三份 profile 对比
> 见 `AGENTS.md` 的「P0/P1 性能优化实施记录」。


### Task 1: 合并行级与块级内容哈希

**Files:**
- Modify: `src/core/terminal/TerminalTypes.h`
- Modify: `src/core/terminal/TerminalCore.cpp`
- Modify: `src/renderer/RowBlockDamageTracker.h`
- Modify: `src/renderer/TerminalRenderer.cpp`
- Test: `tests/core/TerminalCoreTests.cpp`
- Test: `tests/renderer/RendererP3Tests.cpp`

**Interfaces:**
- Produces: RendererSnapshot中每行一次计算得到的8列块哈希和由其折叠得到的整行identity。
- Consumes: 现有 `dirtyRows`、row revision和live-scroll identity恢复逻辑。

- [x] **Step 1:** 新增测试，断言脏行snapshot同时提供稳定row identity与block identities，默认补白和历史切片语义一致。
- [x] **Step 2:** 运行Core/Renderer测试，确认新字段缺失导致RED。
- [x] **Step 3:** 在Core单次Cell遍历中生成block hashes，并由block hash序列生成row identity。
- [x] **Step 4:** 让 `RowBlockDamageTracker::reconcileRow` 消费snapshot提供的block hashes，删除Renderer重复Cell扫描。
- [x] **Step 5:** 运行 `novaterm_core_tests` 与 `novaterm_renderer_tests`，确认GREEN并检查live-scroll/漏绘回归。

### Task 2: 收窄GPU instance暂存与上传准备

**Files:**
- Modify: `src/renderer/TerminalRenderer.h`
- Modify: `src/renderer/TerminalRenderer.cpp`
- Test: `tests/renderer/RendererP3Tests.cpp`
- Benchmark: `tests/benchmarks/RendererP5GpuBenchmark.cpp`

**Interfaces:**
- Produces: 可复用的背景/content instance scratch区，只初始化实际上传范围。
- Consumes: `RenderCommandRow`、dirty spans、row slot与现有QRhi partial update接口。

- [x] **Step 1:** 新增纯CPU辅助测试，覆盖背景完整槽、content每Cell四槽、局部span offset与空槽清零。
- [x] **Step 2:** 运行Renderer测试，确认辅助接口缺失导致RED。
- [x] **Step 3:** 将span到 `GpuInstance` 的装配抽成可测试函数；背景直接覆盖，content仅清理实际span。
- [x] **Step 4:** 复用scratch容量，避免每span `QList::fill` 重新增长；保留一次QRhi staging copy。
- [x] **Step 5:** 跑Renderer测试与GPU benchmark，比较 `contentUploadBytes`、CPU frame P95和memmove占比。

### Task 3: 消除RenderCommand过量扩容与全量stable_sort

**Files:**
- Modify: `src/renderer/TerminalRenderer.cpp`
- Modify: `src/renderer/RenderCommandBuffer.h`
- Test: `tests/renderer/RendererP3Tests.cpp`

**Interfaces:**
- Produces: 按cellColumn稳定有序的背景/content命令。
- Consumes: 旧行未脏命令和按列生成的新命令。

- [x] **Step 1:** 新增增量列span测试，断言保留旧命令与新命令合并后顺序、类型和列号正确。
- [x] **Step 2:** 运行Renderer测试确认旧实现的分配/排序统计不满足新期望。
- [x] **Step 3:** content按最坏四命令/Cell预留容量；背景按一命令/Cell预留。
- [x] **Step 4:** 将两个已有序序列线性merge，删除两次全量 `stable_sort`。
- [x] **Step 5:** 运行Renderer测试与CPU/GPU benchmark，核对命令数和图像一致性。

### Task 4: 收窄Glyph稳态QString/QHash工作

**Files:**
- Modify: `src/renderer/font/FontManager.h`
- Modify: `src/renderer/font/FontManager.cpp`
- Modify: `src/renderer/TerminalRenderer.cpp`
- Test: `tests/renderer/RendererP5Tests.cpp`

**Interfaces:**
- Produces: 一次字体选择同时生成GlyphKey，并让cache miss复用同一FontSelection。
- Consumes: font generation、cluster、bold/italic、cell span、scale和render mode。

- [x] **Step 1:** 新增统计测试，断言一次 `ensureGlyph` miss不重复执行字体选择。
- [x] **Step 2:** 运行P5测试确认RED。
- [x] **Step 3:** 增加返回 `GlyphKey + FontSelection` 的窄接口，删除miss路径第二次 `select`。
- [x] **Step 4:** 为单ASCII码点加有界直接缓存，font generation变化时整体失效。
- [x] **Step 5:** 运行P5测试和冷/暖glyph benchmark，确认栅格化与atlas语义不变。

### Task 5: 批量化moverect目标区域同步

**Files:**
- Modify: `third_party/libvterm-0.3.3/include/vterm.h`
- Modify: `third_party/libvterm-0.3.3/src/screen.c`
- Modify: `src/core/terminal/VTAdapter.cpp`
- Test: `tests/core/TerminalCoreTests.cpp`
- Test: `tests/benchmarks/CoreBenchmark.cpp`

**Interfaces:**
- Produces: 仅VTAdapter实现使用的矩形/行批量Cell读取接口，输出仍为 `VTermScreenCell`。
- Consumes: moverect回调给出的最终destination矩形。

- [x] **Step 1:** 扩展现有batched/incremental输入测试，覆盖重叠滚动、后续擦除、宽字符与属性。
- [x] **Step 2:** 临时切换到批量接口调用并确认接口缺失导致RED。
- [x] **Step 3:** 在libvterm边界内逐行批量填充外部Cell，复用行缓冲，减少公开API调用与坐标检查。
- [x] **Step 4:** VTAdapter按行转换并写入ScreenBuffer，保持最终目标区域语义。
- [x] **Step 5:** 跑Core测试、ASan/UBSan和20 MiB/10万行benchmark。

### Task 6: 移除40 ms dock hover轮询

**Files:**
- Modify: `src/ui/app/MainWindow.h`
- Modify: `src/ui/app/MainWindow.cpp`
- Test: 无现成UI覆盖；使用编译、实际窗口和pidstat/perf验收。

**Interfaces:**
- Produces: 基于鼠标移动、leave、dock resize和窗口状态事件的高亮刷新。
- Consumes: 现有 `updateDockResizeHighlight` 与 `DockResizeHighlight`。

- [x] **Step 1:** 记录静态窗口主线程cswch/s和40 ms timer触发频率基线。
- [x] **Step 2:** 在MainWindow及相关dock/central widget安装事件过滤，覆盖MouseMove、Leave、Resize、Move、WindowStateChange。
- [x] **Step 3:** 删除常驻 `resizeHoverTimer`，拖动期间需要持续跟踪时仅启动临时单次检查。
- [x] **Step 4:** 编译并实跑左右dock高亮、离开隐藏、拖动和窗口最小化恢复。（用户已于 2026-09-13 在窗口环境人工验收完成）
- [x] **Step 5:** 用pidstat/perf复测空闲主线程唤醒。

### Task 7: 文档与最终回归

**Files:**
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/architecture/Rendering_Architecture.md`
- Modify: `docs/architecture/stages/P1_ScreenBuffer_and_VTAdapter.md`
- Modify: `docs/architecture/stages/P5_Glyph_and_GPU_Pipeline.md`
- Modify: `AGENTS.md`

- [x] **Step 1:** 记录每项实现、A/B数据、未达标项和证据位置。
- [x] **Step 2:** 运行RelWithDebInfo全工程构建。
- [x] **Step 3:** 运行全部CTest并单列既有UI失败。
- [x] **Step 4:** 运行ASan/UBSan Core/Renderer相关测试。
- [x] **Step 5:** 重录perf并与当前 `/home/super/perf.data` 的Top 20对比。
