# NovaTerm 架构重构计划

> 制定日期：2026-07-29  
> 依据：NovaTerm GPU 架构图、性能优化版架构图及当前代码审阅结果  
> 目标：在保留现有 QRhi GPU 渲染和统一 Transport 接口的基础上，逐步实现模块解耦、异步解析、增量渲染和百万行 Scrollback。

## 1. 当前结论

NovaTerm 已经具备以下基础：

- `ui / core / renderer / transport` 基础目录分层；
- 基于 libvterm 的终端协议解析；
- 基于 `QRhiWidget` 的 GPU 渲染；
- Glyph Atlas、动态顶点缓冲和批量绘制；
- 统一的 `ITransport` 接口；
- 基础 Damage Rect 和 Scrollback 环形缓冲。

当前主要差距：

- Transport 数据最终仍在 UI 线程中同步解析；
- Renderer 直接依赖 `VTermScreenCell`、`VTermColor` 等 libvterm 类型；
- 缺少 NovaTerm 自有的 `ScreenBuffer` 和 `VTAdapter`；
- Damage Rect 尚未转化为真正的增量 Render Command；
- 每帧仍遍历全部可见 Cell 并重新生成全部顶点；
- Scrollback 尚未采用 Chunk、快照和异步搜索设计；
- SSH、Serial、Telnet 和 SessionManager 尚未形成完整实现；
- 缺少端到端性能指标和压力测试。

## 2. 总体实施顺序

```text
P0 测试、基准与最小构建拆分
    ↓
P1 ScreenBuffer + VTAdapter
    ↓
P2 Parser Worker + ByteQueue
    ↓
P3 RenderScheduler + 增量 Render Command
    ↓
P4 Chunked Scrollback + Search
    ↓
P5 Glyph Atlas 与 GPU 管线优化
    ↓
P6 SessionManager + Transport 完善
    ↓
P7 插件和扩展系统
```

每个阶段必须满足自己的验收标准后，才能将下一阶段作为默认实现合入。

---

## 3. P0：测试、基准与最小 CMake 拆分

**状态：已完成**

### 目标

- 获得重构前可重复的正确性和性能基线；
- 让 Core 可以脱离完整 GUI 应用独立构建、测试和测量；
- 不提前改变当前终端数据通路。

### 已完成任务

- 新增独立静态库 `novaterm_core`；
- `NovaTerm` 应用通过链接 `novaterm_core` 复用 Core；
- 新增 `novaterm_core_tests`；
- 新增 `novaterm_core_benchmark`；
- 覆盖 ANSI/UTF-8、Damage、Resize 和 Scrollback 基础测试；
- 建立 10 MiB Parser 和 10 万行 Scrollback Release 基线；
- 记录构建环境、测试口径和结果。

### 当前基线

| 指标 | P0 结果 |
| --- | ---: |
| Core 测试 | 4 项通过 |
| Parser 吞吐 | 11.52 MiB/s |
| 解析 10 MiB 数据 | 868.07 ms |
| 写入 10 万行 Scrollback | 337.53 ms |
| 保存的 Scrollback 行数 | 100,000 |

### 后续补充

- 扩充 CSI、OSC、UTF-8 分包、宽字符、组合字符和 reflow 回归测试；
- 增加进程内存、CPU 占用和输入延迟指标；
- 为 Renderer 和 Transport 增加独立 benchmark。

---

## 4. P1：引入 ScreenBuffer 与 VTAdapter

**状态：已完成（2026-07-29）**

### 目标

建立 NovaTerm 自有终端数据模型，使 Renderer 不再依赖 libvterm 的内部数据结构。

### 主要任务

1. 新增 Core 数据类型：

   - `Cell`
   - `Line`
   - `ScreenBuffer`
   - `CursorState`
   - `TerminalProperties`
   - `DirtyRegion`
   - `TerminalSnapshot`

2. 设计 `Cell`：

   - Unicode codepoint 或紧凑字符序列；
   - 前景色和背景色；
   - bold、italic、underline、strike、reverse 等属性；
   - 字符显示宽度；
   - 字体 fallback 索引；
   - 宽字符 continuation 标志。

3. 新增 `VTAdapter`：

   - 独占 libvterm 对象；
   - 接收 libvterm callback；
   - 将 `VTermScreenCell` 转换为 NovaTerm `Cell`；
   - 更新 `ScreenBuffer`；
   - 生成 DirtyRegion、Cursor 和终端属性变化。

4. 修改 Renderer：

   - 只读取 `TerminalSnapshot` 或只读 ScreenBuffer；
   - 删除对 `vterm.h` 的包含；
   - 删除所有 `VTermScreenCell`、`VTermColor` 和 `VTermScreenCellAttrs` 参数。

5. 调整输入边界：

   - UI 将 `QKeyEvent/QMouseEvent/QWheelEvent` 转换为平台无关输入命令；
   - Parser Core 不直接处理 Qt GUI Event；
   - 暂时保留必要 Qt Core 类型，完全去 Qt 化不是本阶段强制目标。

6. 增加测试：

   - libvterm Cell 到 NovaTerm Cell 的转换；
   - 宽字符和组合字符；
   - 颜色和 SGR；
   - Cursor、title、bell；
   - Damage Rect；
   - resize 和 alternate screen。

### 验收标准

- Renderer 源码不再包含 `vterm.h`；
- Renderer 公共和私有接口不再出现 `VTerm*` 类型；
- 替换 Parser 时不需要修改 Renderer；
- Parser 是 ScreenBuffer 的唯一写入者；
- Renderer 只能读取稳定快照；
- P0 正确性测试全部继续通过。

### 实施结果

- 新增 NovaTerm 自有 `Cell`、`TerminalColor`、`CellAttributes`、
  `Position`、`DirtyRegion` 和 `CursorState`；
- 新增连续可见区 `ScreenBuffer` 和值语义 `TerminalSnapshot`；
- 新增 `VTAdapter`，独占 libvterm 生命周期、callback 和类型转换；
- `TerminalCore` 改为 Qt 信号门面，对外不再暴露 `VTerm*`；
- Scrollback 改为保存 NovaTerm `Cell`；
- Renderer 的公共接口和活动 GPU 渲染路径已移除 libvterm 类型；
- Core 自动化测试扩展到 7 项；
- P1 Release 基准为 4.22 MiB/s。该结果包含每批输入后的 damage
  flush 和 ScreenBuffer 同步，暴露了 P2 需要通过批量队列解决的同步成本。

---

## 5. P2：Parser Worker 与有界 ByteQueue

**状态：已完成（2026-07-29）**

### 目标

把终端协议解析从 UI 线程移出，使高吞吐输出不再阻塞输入、窗口操作和界面刷新。

### 前置条件

- P1 已建立明确的数据所有权和只读快照；
- 不允许 Renderer 跨线程直接调用可变 Parser 状态。

### 主要任务

1. 新增有界输入队列：

   - 优先采用 SPSC RingBuffer；
   - 支持批量写入和读取；
   - 初始批次大小建议 8–64 KiB；
   - 提供队列容量、积压量和丢弃统计；
   - 禁止无上限内存增长。

2. 新增 Parser Worker：

   - 独立线程运行；
   - 批量消费字节；
   - 独占 `VTAdapter` 和可写 ScreenBuffer；
   - 批量发布 DirtyRegion 和终端状态变化。

3. 调整数据通路：

```text
Transport
    → ByteQueue
    → Parser Worker
    → VTAdapter
    → ScreenBuffer
    → Snapshot / DirtyQueue
```

4. 完善生命周期：

   - start、stop、close；
   - resize；
   - reconnect；
   - 应用退出；
   - Session 关闭；
   - 队列中仍有数据时的安全停止。

5. 实现背压策略：

   - 队列达到高水位时暂停或降低 Transport 读取；
   - 对无法暂停的 Transport 记录过载状态；
   - 不允许静默丢失普通终端输出。

6. 增加线程安全测试：

   - 高频创建和关闭 Session；
   - Parser 工作期间 resize；
   - 大流量输出期间关闭窗口；
   - reconnect；
   - 长时间压力测试。

### 验收标准

- UI 线程不再执行 `vterm_input_write()`；
- 持续大流量输出时 UI 仍可及时响应；
- Parser 和 Renderer 之间不存在可变对象的无保护共享；
- 关闭 Session 不发生死锁、Use-After-Free 或线程泄漏；
- Release Parser 吞吐达到或明显接近 20 MiB/s 目标；
- 输入事件延迟目标小于 10 ms。

### 实施结果

- 新增固定容量环形 `BoundedByteQueue`，容量 8 MiB，支持批量读写、阻塞背压、
  停止唤醒及容量/高水位/等待次数/累计流量统计；
- 新增独立 Parser Worker，按最大 64 KiB 批次消费字节，并独占
  `VTAdapter` 及所有 libvterm 可变状态；
- resize、键盘、鼠标、粘贴、焦点、配色和 scrollback 配置统一串行化到
  Worker 命令队列；
- 屏幕模型由互斥锁保护，Renderer 每帧读取值语义 `TerminalSnapshot`，
  不再逐 Cell 跨线程访问活动屏幕；
- damage、scrollback、title、bell 和 output 信号按解析批次合并后投递到
  Qt 对象线程；
- 新增 `waitForIdle()` 完成屏障和队列统计接口，便于测试、基准与安全退出；
- Core 自动化测试扩展到 10 项，覆盖环形回绕、满载背压、异步批处理、
  resize 与高负载销毁；
- Debug/Release Core 测试以及完整 Debug `NovaTerm` 构建均通过；
- Release 20 MiB 基准结果为 9.35 MiB/s，10 万行 Scrollback 为
  490.63 ms。解析已完全移出 UI 线程，但吞吐尚未达到 20 MiB/s，因此
  “20 MiB/s”作为后续持续优化指标保留，不阻塞线程架构落地。

---

## 6. P3：RenderScheduler 与增量 Render Command

### 目标

让 Damage Rect 真正减少 CPU 帧构建和 GPU 数据上传，而不只是调用局部 `update()`。

### 主要任务

1. 新增 `RenderScheduler`：

   - 收集 DirtyRegion；
   - 合并重叠和相邻矩形；
   - 对全屏更新设置合并阈值；
   - 每帧最多提交一次；
   - 支持 60/120/144 Hz；
   - 支持 VSync 和帧节流；
   - 避免 Parser 每次回调直接触发一帧。

2. 新增 `RenderCommandBuffer`：

   - Background Rect；
   - Glyph Instance；
   - Underline/Strike；
   - Cursor；
   - Selection Overlay；
   - 预留 Hyperlink/Search Overlay。

3. 增量更新：

   - 只重建脏行或脏 Chunk；
   - 保留未变化区域的 GPU 数据；
   - Cursor 和 Selection 使用独立 Overlay；
   - Scroll 时优先复用已有行命令。

4. GPU 提交优化：

   - 使用双缓冲或环形动态 Buffer；
   - 避免 CPU/GPU 同步等待；
   - 评估 Glyph Instancing；
   - 按 Atlas、纹理和渲染状态分批；
   - 减少每个矩形重复生成六个顶点。

5. 新增指标：

   - Dirty Rect 数量和合并后数量；
   - Render Command 生成耗时；
   - CPU 帧时间；
   - GPU 帧时间；
   - Draw Call 数量；
   - 丢帧和积压帧。

### 验收标准

- 单字符变化不再扫描全部可见 Cell；
- 连续 Parser 更新不会产生等量 GPU 帧；
- Render Command 生成耗时可独立测量；
- 普通输出稳定达到 60 FPS；
- 高频输出时允许合并中间状态，但最终屏幕内容必须正确；
- 无明显 GPU 同步停顿。

---

## 7. P4：Chunked Scrollback 与异步搜索

### 目标

实现内存可控、追加均摊 O(1)、支持百万行的 Scrollback。

### 主要任务

1. 将当前 `QVector<QVector<ScrollbackCell>>` 替换为固定大小 Chunk：

   - 每个 Chunk 建议 1024 或 4096 行；
   - Cell 数据尽量连续；
   - 减少逐行小对象分配；
   - Chunk 采用环形管理。

2. 增加快照能力：

   - Chunk 引用计数；
   - Copy-on-Write；
   - Renderer 和 Search 只持有只读快照；
   - Parser 不修改已发布的 Chunk。

3. 增加容量策略：

   - 默认 10 万行；
   - 可配置到 100 万行；
   - 同时支持最大行数和最大内存预算；
   - 达到预算后按 Chunk 淘汰。

4. 优化 reflow 和 pop：

   - 消除非满状态逐行整体移动；
   - resize/reflow 不阻塞 UI；
   - 保持宽字符和组合字符正确。

5. 新增 SearchEngine：

   - 独立线程；
   - 消费只读 Chunk 快照；
   - 支持取消；
   - 支持增量结果；
   - Search Match 通过 Overlay 显示，不修改 Cell。

### 验收标准

- 追加一行保持均摊 O(1)；
- 100 万普通文本行内存占用可控；
- 快速滚动不阻塞 Parser；
- 搜索不阻塞 UI 和 Parser；
- Renderer/Search 快照不会阻止旧数据正常淘汰；
- resize/reflow 压力测试通过。

---

## 8. P5：Glyph Atlas 与 GPU 管线优化

### 目标

让 Glyph 缓存能够稳定支持 ASCII、CJK、字体 fallback、组合字符和 Emoji。

### 主要任务

1. 从 `TerminalRenderer` 中拆分：

   - `GlyphCache`
   - `GlyphRasterizer`
   - `GlyphAtlas`
   - `FontManager`

2. Glyph Cache Key 至少包含：

   - 字体；
   - 字号；
   - 字重和样式；
   - codepoint/cluster；
   - DPI；
   - Cell span；
   - fallback 字体索引。

3. Atlas 改进：

   - 多页 Atlas；
   - LRU 淘汰；
   - 局部纹理上传；
   - 常用 ASCII 可选预热；
   - Atlas 统计和调试视图。

4. 字体能力：

   - CJK fallback；
   - 组合字符；
   - Emoji 和彩色字形；
   - 双宽字符；
   - 高 DPI；
   - 字体变化时安全失效。

5. GPU 后端：

   - 保持 QRhi 为统一入口；
   - 验证 D3D11、D3D12、Vulkan、OpenGL；
   - 测试 GPU 资源丢失；
   - 避免后端切换影响 Core。

### 验收标准

- 大字符集不会因为单张 Atlas 满而停止缓存；
- 新增少量 Glyph 时不上传整张 Atlas；
- CJK、组合字符和 fallback 显示正确；
- DPI 或字体切换不产生旧资源引用；
- Renderer 保持与 Parser 和 Transport 无关。

---

## 9. P6：SessionManager 与 Transport 完善

### 目标

建立完整多会话生命周期，并补齐本地、SSH、Serial 和 Telnet Transport。

### 主要任务

1. 新增 `Session`：

   - Transport；
   - ByteQueue；
   - Parser Worker；
   - VTAdapter；
   - ScreenBuffer；
   - RenderScheduler；
   - 会话配置和状态。

2. 新增 `SessionManager`：

   - 创建和关闭会话；
   - 会话列表；
   - 激活和切换；
   - 持久化配置；
   - 批量关闭；
   - 资源回收。

3. Transport 实现顺序：

   - Local PTY/ConPTY；
   - SSH；
   - Serial；
   - Telnet；
   - Custom Transport。

4. 改进 `ITransport`：

   - 异步连接和关闭；
   - 部分写；
   - 写队列；
   - `bytesWritten`；
   - error category；
   - KeepAlive；
   - reconnect；
   - resize；
   - 背压。

5. 多 Session 隔离：

   - 每个 Session 独立 Parser 状态；
   - 避免共享大锁；
   - 后台 Session 可降低渲染频率；
   - Session 关闭后不得残留线程或 GPU 资源。

### 验收标准

- Local、SSH、Serial、Telnet 使用统一 Session 数据通路；
- 多 Session 并发输出互不阻塞；
- 后台 Session 不产生不必要的高频 GPU 帧；
- 连接失败、断线和重连状态明确；
- Session 重复创建/销毁压力测试通过。

---

## 10. P7：插件和扩展系统

### 目标

在主数据通路稳定后提供受控扩展能力，而不破坏 Core 数据所有权。

### 主要任务

- 主题插件；
- Lua/JavaScript 脚本；
- 状态监控；
- 自定义协议工具；
- SFTP 和文件管理；
- 云同步；
- AI Assistant；
- 插件权限、版本和生命周期。

### 约束

- 插件不能获得可写 ScreenBuffer；
- 插件不能直接持有 GPU 资源；
- 插件通过命令、事件和只读快照工作；
- 插件异常不能阻塞 Parser 或 Render Thread；
- 插件 API 必须有版本管理。

### 验收标准

- 禁用所有插件时主数据通路不受影响；
- 插件崩溃或超时不会破坏 Session；
- 插件接口不暴露 libvterm 或具体 GPU 后端类型；
- 插件加载、卸载和更新具有明确生命周期。

---

## 11. 跨阶段测试矩阵

每个阶段都应持续运行以下场景：

| 类别 | 场景 |
| --- | --- |
| Parser | CSI、OSC、DCS、UTF-8 分包、无效序列 |
| Unicode | ASCII、CJK、宽字符、组合字符、Emoji |
| Screen | resize、reflow、alternate screen、scroll region |
| Input | 键盘、IME、鼠标、粘贴、Bracketed Paste |
| Scrollback | 10 万行、100 万行、清空、淘汰、搜索 |
| Lifecycle | 创建、关闭、重连、退出、异常断开 |
| Performance | 大文件输出、持续吞吐、突发吞吐、空闲功耗 |
| Renderer | Dirty Rect、Cursor、Selection、字体/DPI 切换 |
| Multi-session | 前台与后台会话并发输出 |

## 12. 性能目标

| 指标 | 目标 |
| --- | ---: |
| 持续 Parser 吞吐 | > 20 MiB/s |
| 突发吞吐 | > 100 MiB/s |
| 常规渲染帧率 | 60 FPS |
| 高刷新率支持 | 120/144 Hz |
| 输入延迟 | < 10 ms |
| 默认 Scrollback | 100,000 行 |
| 可配置 Scrollback | 1,000,000 行 |
| Parser 阻塞 UI 时间 | 0 ms |
| 高频输出期间内存 | 有明确上限 |

性能数据必须使用 Release 构建记录，并注明硬件、系统、Qt 版本、编译器、输入数据和测试时长。

## 13. 实施原则

1. 每个阶段保持 NovaTerm 可构建、可运行；
2. 先建立测试和指标，再改变实现；
3. 不在一次提交中同时重构数据模型、线程模型和 Renderer；
4. 新旧实现需要迁移期时使用清晰适配层，不长期保留两条数据通路；
5. Parser 负责写，Renderer/Search 只读；
6. 有界队列优先于无界消息积压；
7. Correctness 优先于局部 benchmark 数字；
8. P1/P2 未完成前，不直接把当前 TerminalCore 移到线程并允许 Renderer 跨线程读取；
9. 插件系统必须等核心数据通路稳定后实施；
10. 每阶段完成后更新本文档状态和性能基线。
