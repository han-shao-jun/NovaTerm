# P3 RenderScheduler 与增量 Render Command 具体实现方案

> 日期：2026-07-30  
> 依据：`NovaTerm_Architecture_Refactoring_Plan.md` 的 P3 要求  
> 范围：只改造渲染调度、CPU Render Command 构建和 GPU 顶点上传；不改动 P2 Parser Worker
> 所有权模型，也不提前实施 P4/P5。

## 1. 现状与问题

当前 `TerminalCore::damage()` 虽然会换算为局部 `QRhiWidget::update(rect)`，但
`TerminalRenderer::render()` 仍然会：

1. 获取完整 `TerminalSnapshot`；
2. 扫描全部可见 Cell；
3. 重新生成全部背景、字形和装饰顶点；
4. 通过一次 `updateDynamicBuffer()` 上传全部顶点。

因此 Damage Rect 目前只缩小了 Qt 的更新区域，没有减少 CPU 帧构建和 GPU 数据上传。
此外，Parser 每个已发布批次都可立即请求一次刷新，缺少明确的帧合并边界。

## 2. 落地架构

```text
TerminalCore::damage / cursorMoved / scrollbackChanged
        │
        ▼
RenderScheduler
  - 收集、裁剪和合并 DirtyRegion
  - 相邻/重叠矩形合并
  - 超过区域数或覆盖率阈值时升级为全屏
  - 以 60/120/144 Hz 帧间隔最多提交一次
        │ frameRequested
        ▼
TerminalRenderer
  - 一帧只取一次稳定 TerminalSnapshot
  - 仅为脏行重建 RenderCommandBuffer
  - 光标和选区作为独立 Overlay 每帧按需重建
        │
        ▼
固定槽位 QRhi Dynamic Vertex Buffer
  - 背景区：每行 columns × 6 vertices
  - 内容区：每行 columns × 24 vertices
  - Overlay 区：selection rows + cursor
  - 仅对脏行槽位调用 updateDynamicBuffer()
  - 背景批次、内容批次、Overlay 批次依次 draw
```

## 3. 新增组件

### 3.1 RenderScheduler

位置：`src/renderer/RenderScheduler.{h,cpp}`

职责：

- 接收并裁剪 `DirtyRegion`；
- 合并重叠或边缘相邻区域；
- 默认在区域数超过 32，或覆盖面积达到屏幕面积 60% 时升级为全屏；
- 用 `Qt::PreciseTimer` 按目标刷新率节流；
- 一个帧间隔内不论收到多少次 Parser damage，最多发出一次 `frameRequested`；
- 支持 60/120/144 Hz，非法值回退到 60 Hz；
- 提供输入矩形数、合并后矩形数、请求帧、合并帧和全屏帧计数。

全屏失效用于 resize、主题、字体、滚动位置、scrollback 结构和 GPU 资源重建。光标与选区变化
使用 overlay 失效，不把内容行标记为脏。

### 3.2 RenderCommandBuffer

位置：`src/renderer/RenderCommandBuffer.{h,cpp}`

每个可见行分别缓存：

- `BackgroundRect`
- `GlyphInstance`
- `Underline`
- `Strike`

Overlay 独立缓存：

- `Cursor`
- `SelectionOverlay`
- 预留 `HyperlinkOverlay`
- 预留 `SearchOverlay`

命令只描述逻辑矩形、Atlas UV 和颜色，不持有 QRhi 资源。这样命令生成时间可以独立测量，
也不会把具体 GPU 后端类型泄漏到 Core。

## 4. TerminalRenderer 改造

1. `damage` 不再直接调用 `update(rect)`，而是提交给 `RenderScheduler`。
2. `frameRequested` 到达时保存合并后的区域并仅调用一次 `update()`。
3. 首帧、resize、字体/主题/滚动/scrollback 变化执行全屏失效。
4. 普通 damage 只重建与区域相交的可见行，不扫描其他行。
5. 每行命令写入固定 GPU 槽位：
   - 背景最多每 Cell 一个 Quad（6 顶点）；
   - 内容最多每 Cell 四个 Quad（字形、两条下划线、删除线，共 24 顶点）；
   - 宽字符 continuation 不生成命令。
6. GPU Buffer 容量仅在行列数、DPR 或资源生命周期变化时重分配。
7. 只上传脏行的背景和内容顶点区间；Overlay 独立上传。
8. draw 顺序固定为全部背景、全部内容、Overlay，避免后一 Cell 背景覆盖前一字形的抗锯齿边缘。
9. Glyph Atlas 新增字形时，本阶段仍上传整张 Atlas；Atlas 局部上传和多页管理留给 P5。

## 5. 指标

`TerminalRenderer::renderStatistics()` 暴露以下累计值：

- 原始/合并 Dirty Rect 数；
- 调度帧、被合并的帧请求、全屏帧；
- 重建行数；
- Render Command 数；
- Render Command 生成纳秒数；
- CPU render 纳秒数；
- GPU buffer 上传字节数；
- draw call 数；
- 顶点缓冲重分配次数。

QRhi 当前没有跨后端、无阻塞的统一 GPU timestamp 查询。本阶段不为获取 GPU 时间引入同步等待；
以 draw call 和上传字节作为 GPU 提交侧指标，真实 GPU 帧时间在后续接入后端 timestamp query。

## 6. 正确性与退化策略

- Damage 为空、越界或尺寸变化时安全裁剪/全屏重建。
- 脏行命令超过固定槽位容量时，本帧记录溢出并退化为重新分配更大容量，而不是截断内容。
- Scrollback 视图发生结构变化时全屏失效，避免 screen row 与 document row 映射失配。
- 位于实时底部时，scrollback 追加复用同一 Parser 批次发布的 active-screen damage，
  不额外请求全屏；只有回看历史或历史映射被裁剪时才全屏失效。
- GPU 资源丢失后保留 CPU 命令语义，但强制所有行重新上传。
- 高频 damage 可合并中间帧，但下一次调度读取的是最新稳定快照，最终画面与 Core 一致。

## 7. 测试与验收

新增 renderer support 单元测试：

1. 重叠、相邻 DirtyRegion 合并；
2. 大面积/过多区域升级全屏；
3. 高频请求在一个帧间隔内只产生一次提交；
4. RenderCommandBuffer 只替换指定行，未变行命令保持不变；
5. resize 会清空不再有效的行缓存；
6. 命令计数和指标正确。

构建验证：

```powershell
cmake --build cmake-build-debug --target novaterm_core_tests novaterm_renderer_tests NovaTerm
ctest --test-dir cmake-build-debug --output-on-failure
```

验收对应：

- 单字符 damage 仅重建一行；
- 连续 Parser 更新按目标刷新率合并；
- 命令生成耗时可独立读取；
- 未变化行不重新生成、不重新上传；
- 最终完整屏幕内容仍来自同一份 `TerminalSnapshot`；
- Core 原有测试全部继续通过。
