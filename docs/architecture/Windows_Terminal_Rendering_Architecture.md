# Windows Terminal 渲染架构（对照参考）

> 来源：本地源码 `E:\code\Qt\terminal`，当前提交
> `805068781c6fed71ef6e2ac85f759e37b954449b`（2026-10-05）。
> 整理日期：2026-10-06。第 1～5 节原基于 `2b5336c1f`（2026-09-30）；
> 已核对两提交之间 `src/renderer/` 无差异，原渲染行号仍适用。
> 第 6～10 节按当前提交补充，新增行号以当前提交为准。
> 用途：为 NovaTerm 渲染优化提供对照，不是 NovaTerm 自身设计。
> 标注“推断”的条目未在源码中直接核实。

## 1. 总览

```
TextBuffer ──(控制台锁内)──> Renderer (src/renderer/base/renderer.cpp)
                               │  拆成 GDI 风格图元：换画笔 / 画一段字符 / 画网格线 / 画光标
                               ▼
                         IRenderEngine (src/renderer/inc/IRenderEngine.hpp)
          ┌────────────────────┼────────────────────┬──────────────┐
          ▼                    ▼                    ▼              ▼
     AtlasEngine           GdiEngine             UiaEngine      WddmConEngine
  (atlas/, 默认引擎)     (gdi/, 传统 conhost)   (uia/, 无障碍)   (wddmcon/)
          │
          │ 锁内：AtlasEngine.cpp / .api.cpp  → 组装 RenderingPayload
          │ 锁外：AtlasEngine.r.cpp           → 交换链 + IBackend::Render + Present1
          ▼
   IBackend ── BackendD3D（自研字形 atlas，GPU 实例化绘制）
            └─ BackendD2D（纯 Direct2D，远程桌面 / 无 GPU / 老 GPU）
```

核心特点：

- 单独的渲染线程；靠 `WaitOnAddress` 与计时器驱动，没有固定 FPS 上限，帧节奏来自
  交换链的帧延迟可等待对象（`SetMaximumFrameLatency(1)`）和 vsync Present。
- 控制台锁只覆盖“读取缓冲 → 组装 payload”这一段；GPU 提交与 `Present` 在锁外。
  AtlasEngine 因此被拆成 `_api`（锁内写）与 `_p`（锁外读）两份状态。
- 失效粒度是**整行**；滚动通过旋转行指针 + `Present1` 滚动矩形复用像素。
- 颜色不随字形走：每帧维护三张“每 cell 一像素”的位图（背景/前景/下划线），
  选区与搜索高亮直接改写这些位图。
- 所有设置是分代（generational）的，只比较代号即可判断是否需要重建资源。

README（`src/renderer/atlas/README.md`）作者自评：先拆成 GDI 图元再拼回
DirectWrite 既浪费又容易出 bug，更好的做法是把 TextBuffer 与设置直接交给 AtlasEngine。

## 2. Renderer：线程、计时器与帧调度

### 2.1 线程主循环（renderer.cpp:139-162）

```cpp
while (_threadKeepRunning) {
    _enable.wait();                       // 禁用期间阻塞
    if (!_waitUntilCanRender()) break;    // 各引擎 WaitUntilCanRender(shutdownEvent)
    _waitUntilTimerOrRedraw();            // WaitOnAddress(_redraw) + 最近计时器超时
    if (!_threadKeepRunning) break;
    LOG_IF_FAILED(PaintFrame());
}
```

- 顺序是**先等交换链可接收新帧，再等重绘请求**，所以重绘请求天然被合并到下一 vsync。
- `NotifyPaintFrame()` 只做 `_redraw.store(true)` + `WakeByAddressSingle`，代价极低，
  可以在 VT 解析热路径上随意调用。
- `_redraw` 不在等待处清零，而是在拿到控制台锁、`_tickTimers()` 之后清零：
  计时器回调触发的通知被本帧吸收，之后的请求仍会触发下一帧。
- 计时器（光标闪烁、blink 属性 1 s）用 `QueryUnbiasedInterruptTime`，到期后
  `next += interval` 防漂移；改动计时器时 notify 一次让等待重算超时。
- 连续输出时（buffer/cursor mutation id 变化）光标强制常亮并停掉闪烁计时器。

### 2.2 重试与退避（renderer.cpp:357-400）

- 每帧最多尝试 6 次，失败后等待 100/200/400/800/1600 ms（注释写的序列不准）。
- `E_PENDING`（设备丢失）不写日志；重试时 `TriggerRedrawAll()`，因为失败的尝试
  可能已消耗掉失效区域。
- 全部失败则禁用绘制并回调宿主进入错误态——“黑屏好过 abort”。
- 不按引擎单独重试，避免文本引擎与 UIA 状态不一致。

### 2.3 锁的范围（renderer.cpp:402-468）

锁内依次：同步输出等待 → tick 计时器 → 清 `_redraw` → 检查视口与滚动 →
失效旧光标/旧 IME 组合区 → 更新光标 → 失效新光标 → 准备新组合文本 →
对每个引擎 `_PaintFrameForEngine`。出锁后才对需要的引擎调用 `Present()`。

### 2.4 单引擎一帧的调用序列（renderer.cpp:470-539）

1. `StartPaint()`：返回 `S_FALSE` 表示无事可做，跳过本帧全部步骤（含 EndPaint/Present）。
2. `UpdateDrawingBrushes(默认属性)` → `ScrollFrame()` → `PrepareRenderInfo(选区/搜索高亮)`
   → `PaintBackground()`。
3. `_PaintBufferOutput()`：对 `GetDirtyArea()` 的每个矩形逐行输出（见 2.5）。
4. `_PaintSelection()` → `_PaintCursor()` → `_PaintTitle()` → `EndPaint()`。
5. `RequiresContinuousRedraw()` 为真（如带 time 的自定义 shader）时再 notify 下一帧。

### 2.5 行输出与 run 切分（renderer.cpp:1069-1413）

- 每行：`PrepareLineTransform(行属性 DECDWL/DECDHL)` → 逐 run `PaintBufferLine` →
  网格线 → 该行的 sixel `PaintImageSlice`（画在文字之上）。
- run 在属性、URL pattern id、软字体状态变化时断开；**全空格 cell 若视觉等价
  （只差前景色）则不断开**——cmatrix 这类输出因此不会被切成碎片。
- 宽字符右半作 run 起点时左移一列并 `trimLeft`；零宽 cell 至少前进 1 列防死循环。
- IME 组合文本直接临时写进真实 ROW、出作用域还原，渲染路径无需特判。

### 2.6 同步输出 DECSET 2026（renderer.cpp:542-610）

- 渲染线程持锁发现同步中：释放锁 `WaitOnAddress` 等标志清除，累计最多 100 ms，
  超时强制关闭同步模式。
- 关闭同步时 VT 线程主动 `Unlock/Lock` 一次给渲染线程抢锁机会，否则应用
  持续发 2026 会把渲染压到约 10 FPS（注释自称 hack）。

## 3. AtlasEngine 前半段：锁内组装 RenderingPayload

### 3.1 线程约定

- `AtlasEngine.api.cpp`：锁内 setter，只写 `_api`。
- `AtlasEngine.r.cpp`：锁外 Present，只读 `_p`；出现 `_api.` 即可能竞态。
- `AtlasEngine.cpp`：只由 Renderer 调用，是两边的同步点。

### 3.2 失效记录（api.cpp:46-173）

- `Invalidate*` 只合并**行范围** `invalidatedRows`，列被忽略；光标单独记 cell 区域。
- `InvalidateScroll` 是同步语义：累计 `scrollOffset`，平移已有失效区；水平滚动直接全量。
- `GetDirtyArea()` 只返回一个矩形 `{0, rows.start, cols, rows.end}`。

### 3.3 StartPaint（AtlasEngine.cpp:58-263）

1. 窗口尺寸变化写入设置；设置代号与 `_p.s` 不同则 `_handleSettingsUpdate()`：
   字体变化重建 DWrite 轴/格式，cell 数变化重分配行数组与三张颜色位图，然后全量失效。
2. 无失效行、无光标变化、无连续重绘 → 返回 `S_FALSE`，整帧跳过。
3. 全量失效时 `MarkAllAsDirty()`（脏矩形=整个目标，滚动量清零）。
4. **滚动 = 旋转行指针**：`rows` 是 `ShapedRow*` 数组，按 offset 旋转到 scratch 后交换，
   `ShapedRow` 本身不复制；三张颜色位图各 `memmove` 一次，`memcmp` 确有变化才 bump
   对应代号（注释：再省约一半 GPU 负载）。
5. 每个失效行：脏像素矩形并入**该行旧字形范围** `dirtyTop/dirtyBottom`
   （字形可能超出 cell 高度），然后 `Clear()` 该行。

### 3.4 颜色位图

- `colorBitmap` 是一块 32 字节对齐缓冲，切成 bg / fg / ul 三层，每 cell 一个 u32；
  行跨度对齐到 8 个 u32（注释称 memcpy 快 1.5～40 倍）；DECDWL 行每 cell 占 2 像素。
- bg 预乘 alpha，fg 直通 alpha（沿用 D2D 的混合约定）。
- `_fillColorBitmap` 找到第一个不同像素才 bump 该层代号，内容不变则后端不上传。
- 选区、搜索高亮、焦点高亮在 `PaintBufferLine` 里经 `_drawHighlighted` 直接改写位图；
  `PaintSelection` 是空实现，后端没有选区 pass。选区前景按背景亮度自动取黑/白。

### 3.5 文本累积与塑形（AtlasEngine.cpp:486-1247）

- 同一行的多个颜色 run 先累积进 `bufferLine`（附带每码元起始列），换行或
  `EndPaint/PaintCursor` 时 `_flushBufferLine()` 一次塑形——**连字可以跨颜色 run**。
  只有粗体/斜体变化才强制分段（字体不同）。
- 方向隔离符 U+2066..2069 替换为 U+200B。
- 段落切分：内置字形（制表符/方块/Powerline/Legacy Computing）与软字体走
  `_mapBuiltinGlyphs`，存码元而非字形索引，advance 固定 cell 宽。
- 普通文本：`MapCharacters` 字体回退（可变字体走 `IDWriteFontFallback1` 带轴值）→
  `GetTextComplexity` 快路径（简单文本一码元一字形，advance 直接按 cell 宽）→
  复杂文本 `AnalyzeScript + GetGlyphs + GetGlyphPlacements`。
- **网格对齐**：每个 DWrite cluster 的 advance 之和与 `列数×cellW` 的差值全部
  加到该 cluster 最后一个字形上，cluster 内部相对位置不变。
- 相邻同 face 的 `FontMapping` 合并（`MapCharacters` 慢且反复返回同一 face）。
- 找不到字体时用缓存的 U+FFFD 字形，按 cell 区间各出一个。

### 3.6 每行产物 ShapedRow（common.h:466-499）

`mappings[{fontFace, from, to}]` + 等长的 `glyphIndices / glyphAdvances / glyphOffsets / colors`，
外加 `gridLineRanges`（PaintBufferGridLines 只记范围，颜色已在位图里）、sixel `bitmap`、
`lineRendition`、`dirtyTop/dirtyBottom`（像素）。

### 3.7 字体度量（api.cpp:616-876）

- 字体名列表逗号分隔（类 CSS），第一个存在的为主字体，其余加入回退链，最后接系统回退；
  缺失字体经 warningCallback 报告，全缺回退 Consolas。
- cell 宽取 `'0'` 的 advance（CSS `ch`），cell 宽高可被用户覆盖并取整；
  `baseline = round(ascent + (lineGap + cellH - advanceHeight)/2)`，多出/缺少的高度上下均分。
- 下划线/删除线/双下划线/细线宽度全部在这里预算成整数像素；双下划线间隙至少 1.2 pt。
- 默认开启 `liga/clig/calt`，用户可覆盖。

## 4. AtlasEngine 后半段：锁外 Present（AtlasEngine.r.cpp）

### 4.1 Present 流程（r.cpp:32-84）

```cpp
if (!_p.dxgi.adapter) _recreateAdapter();
if (!_b) _recreateBackend();
if (_p.swapChain.generation != _p.s.generation())
    if (!_handleSwapChainUpdate(shutdownEvent)) return S_FALSE;
_b->Render(_p);
_present();
```

- `DEVICE_REMOVED / DEVICE_RESET / D2DERR_RECREATE_TARGET`：清空 dxgi 状态，返回
  `E_PENDING`，由 Renderer 退避重试并全量重绘；下一次 Present 重建适配器，LUID 变化才重建后端。
- 其他错误：丢弃后端，下次重建。

### 4.2 设备与后端选择（r.cpp:102-302）

- 适配器：默认用 `EnumAdapters1(0)`（不按窗口所在显示器选）；WARP 模式找软件适配器。
- 设备标志：`SINGLETHREADED | PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | BGRA_SUPPORT`
  （后者防止 Nvidia 驱动按 CPU 核数开线程，WARP 上去掉）。
- 特性级别 11_1 → 9_1。`Automatic` 时：WARP、FL < 10_0、或 FL 10_x 不支持 CS4.x
  → BackendD2D；其余 → BackendD3D。内置 shader 是 SM 4.0。
- 源码里没有远程会话判断，D2D 后端只在 README 中被描述为适合远程桌面。

### 4.3 交换链（r.cpp:304-446）

| 项 | 值 |
| --- | --- |
| 格式 | `B8G8R8A8_UNORM`，无 MSAA |
| 缓冲数 | 3（截图/拖窗口时可能两个缓冲同时被锁） |
| SwapEffect | `FLIP_SEQUENTIAL`（DWM 要求，配合 Present1 脏矩形支持 Panel Self Refresh）；禁用 Present1 时 `FLIP_DISCARD` |
| Alpha | 透明背景时 `PREMULTIPLIED`，否则 `IGNORE`（允许 independent flip，延迟更低）；HWND 目标强制 `IGNORE` |
| 帧节奏 | `FRAME_LATENCY_WAITABLE_OBJECT` + `SetMaximumFrameLatency(1)` |
| 目标 | HWND，或 DComposition surface handle（WinUI SwapChainPanel，附 96/dpi 逆缩放矩阵） |

- `WaitUntilCanRender`：上一帧 Present 过则等可等待对象；没 Present（无脏区）则固定
  8 ms 节流，防止离屏输出忙等。
- 尺寸变化只 `ResizeBuffers`，目标设置变化才重建交换链；销毁时 `ClearState + Flush`
  强制 D3D11 立即释放（一个 HWND 同时只能挂一个交换链）。

### 4.4 _present（r.cpp:478-539）

- 脏矩形钳到目标范围；为空则不 Present，只 `Flush()`。
- 非全屏脏区：`Present1(1, 0, {1 个脏矩形, 滚动矩形, 滚动偏移})`；
  滚动矩形高度用文本区高度而非目标高度。
- 全屏脏区（首帧、设置变化、整屏滚动）走不带脏矩形的 Present1。
- 注意：D3D 后端**每帧都重画整个后缓冲**，脏矩形/滚动矩形只是给 DWM/PSR 的合成提示，
  不减少 GPU 绘制量。

## 5. BackendD3D（BackendD3D.h/.cpp）

> README 的 D3D 流程图已过时：`_drawGlyphPrepareRetry`、`_drawSelection` 在当前代码中
> 不存在（后者只剩声明）。以下以源码为准。

### 5.1 Render 顺序（cpp:212-244）

```
设置代号变化 → _handleSettingsUpdate
OMSetRenderTargets（Present 会解绑，每帧重绑）
_drawBackground        一个全屏 Background quad
_drawCursorBackground  光标第一部分（文字下方）
_drawText              所有行的字形、网格线、sixel
_flushQuads            先 _drawCursorForeground（光标第二部分），再一次 DrawIndexedInstanced
自定义 shader（可选）
```

整帧通常只有**一次 draw call**；只有 atlas 写满中途 flush 时才多出一次。

### 5.2 实例数据（h:87-100）

`QuadInstance` 20 字节：`u16 shadingType, u8x2 renditionScale, i16x2 position, u16x2 size,
u16x2 texcoord, u32 color(RGBA, UNORM)`。注释称实例缓冲大小对性能和功耗影响最大，
因此坐标用 i16（允许裁出视口外）且结构体不做零初始化。
几何是单位 quad（4 顶点 + 6 索引，IMMUTABLE）+ 硬件实例化；注释比较了 CS、GS、
SRV 手动实例化，HW 实例化在 Nvidia 上约快 50%。

| ShadingType | 名称 | 说明 |
| --- | --- | --- |
| 0 | Background | 从每 cell 背景纹理取色；在字形缓存中表示空白字形 |
| 1 / 2 | TextGrayscale / TextClearType | 覆盖率来自 atlas，shader 做 gamma 与对比度 |
| 3 | TextBuiltinGlyph | 方块/阴影字符，图案在 PS 里生成 |
| 4 | TextPassthrough | 彩色字形、emoji、sixel，直接取 atlas 预乘色 |
| 5～7 | Dotted / Dashed / CurlyLine | 图案全由 PS 生成 |
| 8～10 | SolidLine / Cursor / FilledRect | 纯色 |

### 5.3 背景

- 背景是 `cellsX × cellsY` 的 `R8G8B8A8` 动态纹理，PS 用 `SV_Position / cellSize`
  直接 `Load`（无 sampler），网格外的边缘像素用常量背景色。
- 只有 `colorBitmapGenerations[0]` 变了才 `Map(WRITE_DISCARD)` 逐行上传，
  注释称不上传能让 GPU 负载减半。

### 5.4 字形 atlas

- 一张 `B8G8R8A8` 纹理，同时是 SRV 与 D2D 渲染目标；字形用 D2D `DrawGlyphRun`
  （白色画刷）光栅化进去，D2D 用 gamma 1.0、对比度 0 的“线性”参数，校正全部在 PS 做。
  源码保留了一个 `#if 0` 的纯 DWrite 路径，注释称 D2D 因其暂存上传器快约 2 倍。
- 关闭 D2D 自带的纹理/彩色字形缓存（`SetMaximumTextureMemory(0)` 等），避免双重缓存。
- 装箱：stb_rect_pack。尺寸取 `clamp(min(2×交换链面积, max(95×cell面积, 上次面积×2)),
  128², 8192²)` 并取 2 的幂。
- **淘汰策略：写满即清空并扩容，无 LRU**。流程：`_d2dEndDrawing → _flushQuads`
  （用旧内容画完已排队的实例）→ `_resetGlyphAtlas` → 重新装箱；仍放不下抛错。
- 缓存键：`IDWriteFontFace*` 指针 → 每种行属性（单宽/双宽/双高上/双高下）一张
  `glyphIndex → AtlasGlyphEntry` 的线性探测哈希表；内置字形单独一张，按码点为键。
- 双高行：字形按 2 倍光栅化一次，拆成上下两个缓存条目指向同一 atlas 区域的两半。
- 彩色字形经 `TranslateColorGlyphRun` 识别，按层绘制（COLR/SVG/PNG 等），标记为 Passthrough。
- 过宽字形（连字）超出 cell 阈值时标 `overlapSplit`，绘制时在前景色变化的 cell
  边界切成多个 quad，使连字内部也能变色。

### 5.5 文本 pass（cpp:1108-1234）

- **每帧遍历所有行**，从缓存的 ShapedRow 重新生成实例；CPU 只对失效行重新塑形。
- 字形位置：`l = lrintf((baselineX + advanceOffset) × scaleX) + offset.x`，颜色取 `row->colors`。
- 只有失效行把字形实际范围并入 `dirtyRectInPx`。

### 5.6 混合与 gamma

- 双源混合：`Src = ONE, Dest = INV_SRC1_COLOR`，PS 输出 `{color, weights}`，
  即 `dest = color + dest × (1 − weights)`。ClearType 时 weights 按通道，其余为 `color.aaaa`。
- PS 复刻 DWrite 的 gamma 校正：`EnhanceContrast(a,k) = a(k+1)/(ak+1)`，
  亮字暗底对比度调整、按 gamma 查 13 行系数表的 alpha 校正（dwrite_helpers.hlsl）。

### 5.7 光标两段式

- 第一段：按背景色变化把光标切段，背景用光标色（反色哨兵 `0xffffffff` 时用 `bg ^ 0xffffff`），
  再用 `GetPerceivableColor` 保证与背景可分辨；作为 Cursor quad 画在文字下方。
- 第二段：在 flush 前找出与光标相交的文字实例，把每个实例切成最多 4 块保持原色，
  交集部分改成对比色——光标下的字形变色但不重复绘制；emoji 跳过。

### 5.8 网格线、sixel、自定义 shader

- 横线按颜色游程合并成一个 quad；竖线每 cell 一个。下划线色取 ul 位图。
- 点线/虚线/波浪线在 PS 用 quad 局部坐标解析生成，波浪线用切线近似距离做 AA。
- sixel 行按 revision 缓存进同一 atlas，Passthrough 绘制。
- 自定义 shader（含复古效果）：主 pass 画到离屏纹理，再全屏 quad 后处理；
  反射发现用了 `time` 变量就要求连续重绘；此时脏矩形强制全屏。

## 6. BackendD2D：共享塑形产物，替换实际绘制实现

### 6.1 边界与帧流程（BackendD2D.cpp:29-185）

D2D 后端仍接收同一份 `RenderingPayload`，复用 AtlasEngine 的行失效、
DirectWrite 塑形、字体回退、颜色位图和滚动结果；它不会重新读取 TextBuffer，
也不承担交换链创建与 Present。后两项仍在 `AtlasEngine.r.cpp`。

```text
设置代号变化 → _handleSettingsUpdate
BeginDraw
  _drawBackground
  _drawCursorPart1
  _drawText（字形 → 网格线 → sixel）
  _drawCursorPart2
EndDraw
AtlasEngine::_present
```

- 从交换链 buffer 0 获取 `IDXGISurface`，建立 D2D render target；使用物理像素
  单位，字体 DPI 与文字抗锯齿模式来自共享设置。
- `BeginDraw/EndDraw` 成对出现；中途异常也先调用 `EndDraw` 再上抛。
- `ReleaseResources()` 释放目标及其可选的 DeviceContext4 接口、清空总设置代号，
  保证 resize 后下次 Render 重建目标。
- `RequiresContinuousRedraw()` 恒为 false；这里没有 D3D 后端的自定义 shader pass。
- **D2D 同样每帧画全目标背景、遍历全部可见行**，没有按 `dirtyRectInPx` 对整帧
  绘制设置裁剪。双高行/波浪线的局部 clip 服务于图元形状，不是脏区绘制优化。

### 6.2 背景：cell 位图放大（cpp:129-207）

- 背景 bitmap 尺寸为 `(cols + 2) × (rows + 2)`，四周各留一个像素，填默认背景色，
  防止网格外边缘拉伸成最后一个 cell 的颜色。
- 内部区域只有背景层 generation 变化时才 `CopyFromMemory` 更新。
- BitmapBrush 使用最近邻插值、CLAMP 和 cell 宽高缩放，偏移一个 cell，
  一次 `FillRectangle` 覆盖整个目标。
- 背景使用 `D2D1_PRIMITIVE_BLEND_COPY`，覆盖包括 alpha 在内的旧像素，
  随后恢复 `SOURCE_OVER` 绘制文字。这是透明背景正确性的组成部分。

### 6.3 文字与内置字形（cpp:210-557）

- 对每行的 `FontMapping`，进一步按 `row->colors` 的相同颜色游程切分，构造
  `DWRITE_GLYPH_RUN`，使用缓存的 indices/advances/offsets 调 `DrawGlyphRun`。
  普通字形的光栅缓存交由 D2D 管理，没有 D3D 后端那张自管普通字形 atlas。
- 源码明确解释为何不采用前景 BitmapBrush：多色文本能更快，但少色文本的
  额外离屏处理代价很高。注释中的倍率是作者记录，未在本机复测。
- 彩色字形通过 `TranslateColorGlyphRun` 枚举各层绘制，并累计实际 bounds。
  普通字形也查询 bounds，更新 `dirtyTop/dirtyBottom`；仅失效行把这些范围
  合入本帧呈现脏区。
- 内置方框/Powerline 等字符另有 bitmap 缓存，借 `ID2D1SpriteBatch` 批量绘制，
  与普通字体缓存分离。缺少 SpriteBatch 时仍推进 advance，但不画这些内置字形。
- 双宽/双高行通过 render target transform 与上下半行 clip 实现，行结束恢复状态。

### 6.4 光标与 sixel 的差异（cpp:812-917）

- 指定颜色光标在文字之前绘制；反色哨兵 `0xffffffff` 则在文字之后，用按光标
  尺寸缓存的 mask bitmap 做 `D2D1_COMPOSITE_MODE_MASK_INVERT`。
  它没有 D3D 后端切分文字实例并重设交集颜色的算法。
- sixel 在每行文字与装饰之后绘制；**每次 `_drawBitmap` 都重新 `CreateBitmap`**，
  源码 TODO 明确指出尚未采用 D3D 的 revision 缓存。

| 项目 | BackendD3D | BackendD2D |
| --- | --- | --- |
| 行塑形与颜色数据 | 共享 RenderingPayload | 共享 RenderingPayload |
| 普通字形绘制 | 自管 atlas + quad 实例 | 按字体/颜色游程 DrawGlyphRun |
| 内置字形 | PS 图案等专门路径 | bitmap + SpriteBatch |
| sixel GPU 缓存 | 按 revision 缓存入 atlas | 每次绘制创建 bitmap |
| 反色光标 | 切分文字 quad、调整颜色 | 末尾 MASK_INVERT |
| 自定义 shader | 离屏主 pass + 后处理 | 未实现该 pass |
| 每帧绘制范围 | 全目标 | 全目标 |

“D2D 后端”不等于“纯 CPU”：它使用共享 D3D 设备的 DXGI surface。
是否用软件设备由适配器/WARP 设置决定，与后端选择是两项设置。

## 7. 宿主接线：Terminal、ControlCore 与 XAML

### 7.1 谁持有谁，何时初始化

`Terminal` 实现 `IRenderData`（`Terminal.hpp:55-57`），提供 TextBuffer、视口、
选区/搜索范围、颜色解析及控制台锁。`IRenderData.hpp` 末尾的注释明确表达了
按值快照、尽早释放终端锁的期望；当前实现仍在锁内读取并塑形，不能当成已经
实现不可变 TextBuffer 快照。

`ControlCore` 的初始化分两步（`ControlCore.cpp:153-165, 373-490`）：

1. 构造时先建立 Renderer 并绑定 Terminal，允许 UIA 在 SwapChainPanel 就绪前
   加入引擎。**构造对象不等于启动渲染线程**：线程实际由
   `Renderer::EnablePainting()` 创建（`renderer.cpp:80-95`）。
2. 面板尺寸非零后，在终端写锁内创建 AtlasEngine、注册到 Renderer、更新字体，
   将逻辑尺寸乘 composition scale 得到像素尺寸，再换算 cell 数、调整连接尺寸、
   创建 Terminal 缓冲并设置引擎选项。

AtlasEngine 的交换链实际创建仍在锁外 Present 路径，UI 不直接执行 GPU 绘制。

### 7.2 交换链 handle 如何跨到 UI（ControlCore.cpp:2017-2044）

```text
渲染线程：AtlasEngine 创建 composition surface / swap chain
  → swapChainChangedCallback(sourceHandle)
  → ControlCore 立即 DuplicateHandle，持有独立句柄
  → resume_foreground(dispatcher)
UI 线程：检查 weakThis → 保存 _lastSwapChainHandle → SwapChainChanged
  → TermControl::_AttachDxgiSwapChainToXaml
  → ISwapChainPanelNative2::SetSwapChainHandle
```

复制必须在切线程前发生：原 handle 属于引擎，可能在 UI 回调执行前被释放。
弱引用避免宿主销毁后继续访问对象。`TermControl.cpp:1363-1366` 负责挂接，
交换链变化时经 `RenderEngineSwapChainChanged` 再次挂接（1294-1298）。

像素坐标分工也要分清：面板提供逻辑尺寸与 composition scale，后端按物理像素
画字；交换链矩阵再乘 `96 / dpi`，抵消 XAML 的缩放
（`AtlasEngine.r.cpp:433-445`），避免重复缩放。

### 7.3 拆离、重挂与停止（renderer.cpp:56-125）

- `ControlCore::Detach()` 调 `TriggerTeardown()`，等待渲染线程退出后清理旧 UI
  线程的节流回调；`AttachToNewControl()` 重建 dispatcher/回调，随后由初始化
  路径重新启用绘制（`ControlCore.cpp:286-308`）。
- `TriggerTeardown()` 同时唤醒 shutdown event、重绘等待和 enable event，
  覆盖线程可能停在的各处；等待线程退出、释放线程句柄，最后禁用绘制。
- `EnablePainting()` 强制视口同步，并全量重绘。原因有二：停绘期间 cell 数可能
  变化；上一帧可能在消费失效区域之后、Present 之前被停止。
- `ControlCore` 析构先关闭连接，再显式等待 Renderer 停止
  （`ControlCore.cpp:278-284`）。不能只依赖智能指针析构顺序来保护回调。

### 7.4 错误恢复有两层

第 2.2 节是 Renderer 内部的逐帧重试；还有宿主层兜底：
`ControlCore::_rendererEnteredErrorState()` 第一次收到重试耗尽通知时，在终端
写锁内设置 **Direct2D + 软件渲染 WARP**，重新启用 Renderer；再次失败才向 UI
发出 `RendererEnteredErrorState`（`ControlCore.cpp:170-182`）。

因此“Renderer 重试耗尽就进入最终错误界面”不完整。设备恢复、帧重试、宿主
切换后端是不同层次，后端自动选择也不同于这次故障兜底。

## 8. 缓存与失效：五种状态不能混为一谈

### 8.1 设置代号的含义

`til::generational<T>`（`src/inc/til/generational.h`）内部保存值与 32 位 generation；
`write()` **拿到可写指针时就 bump**，相等判断只比较代号，不比较内容，也不自动
检测实际值是否变化。它不是带锁容器，也不是自动 CoW。

设置是嵌套的：总 Settings 下有 target/font/cursor/misc 四个代号。
setter 通常先比较值，再调用 `_api.s.write()->子设置.write()`；这同时更新总代号
与子代号（如 `AtlasEngine.api.cpp:427-447`）。`StartPaint` 比较总代号，
`_handleSettingsUpdate` 复制 `_api.s` 到 `_p.s`，按子代号重建相关资源，
但**任一设置更新仍会标记全部行失效**（`AtlasEngine.cpp:703-727`）。

### 8.2 分层依赖表

| 状态 | 保存什么 | 更新/失效条件 | 节省什么 |
| --- | --- | --- | --- |
| ShapedRow | 字体映射、字形索引、advance/offset、颜色、装饰与图片 | 行失效、设置变化；滚动时旋转指针复用 | 未脏行的字体回退与塑形 |
| bg/fg/ul CPU 位图 | 每 cell 的有效绘制颜色 | 内容真正变化才 bump 各层代号 | 无变化时的后端上传/处理 |
| D3D 字形 atlas | 光栅覆盖率/彩色像素与条目位置 | 新字形填充；字体变化更新字体依赖；装满重置 | 重复字形光栅化 |
| 交换链目标资源 | backbuffer 的 RTV / D2D target | resize/交换链重建先 ReleaseResources | 维持目标与设备资源的正确关联 |
| 呈现元数据 | dirtyRectInPx、scrollDeltaY | 每帧组装，按旧/新字形与光标范围扩张 | 合成/呈现范围提示 |

`RenderingPayload` 内含可变缓冲、COM 资源和交换链状态
（`common.h:501-595`），不是通用的不可变消息。`_api/_p` 的约定依靠锁内组装与
同一渲染线程随后锁外消费；不能把 `_p` 交给另一个并发绘制线程直接读。

resize 也不等于清空所有缓存：`_resizeBuffers` 释放后端目标引用、清 D3D 状态，
然后 `ResizeBuffers`（`AtlasEngine.r.cpp:424-431`）；D3D 的
`ReleaseResources` 只重置目标视图和总代号（`BackendD3D.cpp:204-209`），
后续按字体、cell 数等依赖选择性更新资源。

### 8.3 脏范围需要覆盖旧墨迹与新墨迹

`StartPaint` 清行之前并入旧 `dirtyTop/dirtyBottom`，后端绘制时再合入失效行的新
字形 bounds。这样斜体、组合音标、超高 fallback 字体改成空格后，原先越出 cell
的像素仍会被呈现更新。只用“新 cell 矩形”会留下旧字形残影。

滚动时连缓存 bounds 也要平移：旋转 ShapedRow 指针后，对所有行的
`dirtyTop/dirtyBottom` 加 `offset × cellHeight`
（`AtlasEngine.cpp:187-198`），不能只换行身份而保留旧像素坐标。

## 9. 三个典型帧：增量发生在哪里

### 9.1 单字符变化

```text
TextBuffer::TriggerRedraw（buffer 坐标）
  → Renderer 裁到视口、转换为视口坐标
  → 各 engine Invalidate；Atlas 合并为整行范围
  → NotifyPaintFrame 合并请求
  → StartPaint 清理失效行并记录旧墨迹范围
  → Renderer 输出失效行，Atlas 重塑形
  → Backend 遍历全视口并绘制，记录新墨迹范围
  → Present1 呈现受影响像素矩形
```

入口与坐标转换见 `textBuffer.cpp:1089-1125`、`renderer.cpp:656-682`。
若两条不相邻行在同一帧失效，Atlas 的 `[start,end)` 会覆盖中间所有行；
它不是保留多个离散行集合或多个 cell 脏矩形的实现。

### 9.2 向上滚一行

明确滚动量经 `TriggerScroll(delta)` 发给全部 engine，并同步移动上一帧选区
（`renderer.cpp:891-900`）。Atlas 累计 delta，旋转已有行指针、移动颜色位图与
墨迹 bounds，重新塑形新露出的底行；但后端仍重画整个目标。
Present1 的 scroll rect/offset 复用属于合成呈现层，不能算成“GPU 只画底行”。

### 9.3 只有光标闪烁

计时器使旧/新光标区域失效，`StartPaint` 可以在没有文本失效行时继续。
`EndPaint` 把失效光标区域并入 dirty rect，即使本帧光标不可见也要更新
（`AtlasEngine.cpp:278-285`）。不必因光标闪烁重新塑形文本，
但 D3D/D2D 的完整绘制仍会执行。

## 10. 对 NovaTerm 的借鉴与边界

以下是**对照分析与建议**，不是已实施的 NovaTerm 改动。NovaTerm 当前事实以
[`../ARCHITECTURE.md`](../ARCHITECTURE.md) 和自身源码为准；特别是稳定渲染快照
已采用共享不可变行，不应按较早文档中的整屏值拷贝描述回退设计。

| 维度 | Windows Terminal 的选择 | NovaTerm 对照与建议 |
| --- | --- | --- |
| 模型读取 | 控制台锁内读取并塑形，锁外 GPU/Present | 保持 Parser 单写与不可变快照；无需复制其锁内塑形方案 |
| CPU 行增量 | 整行 ShapedRow 缓存，连续行范围失效 | 保留已有行/块身份与局部命令重建；衡量脏范围合并扩大多少工作 |
| GPU 提交 | 全视口重新生成实例、全目标绘制 | 已有行槽位局部上传应保留；一次 draw call 与少上传字节是不同目标 |
| 滚动 | 指针旋转 + bounds 平移 + 位图移动 + Present1 提示 | 保留行身份/槽位复用，并验证位置、overlay、旧墨迹范围一起更新 |
| 字形与颜色 | 缓存光栅像素，颜色另行处理 | 避免把选区、搜索色等易变外观绑定进可复用字形缓存 |
| 缓存淘汰 | 单 atlas，写满 flush 后清空/扩容 | NovaTerm 多页/LRU 方案有不同预算目标，不应仅为仿照而替换 |
| 帧调度 | 请求合并 + 交换链可等待对象，无提交时节流 | 借鉴事件合并与空闲不忙等；QRhi/QWidget 的帧生命周期需单独适配 |
| 恢复 | 设备重建、逐帧退避、宿主 D2D/WARP 兜底 | 区分 CPU 数据保留、GPU 资源恢复与最终错误反馈，并验证重新挂接 |

后续评估应分别记录：重塑形/重建行数、可见行扫描量、实例生成量、GPU 上传字节、
atlas miss/重置次数、draw call、实际帧耗时与呈现延迟。源码注释中的“负载减半”、
“快 50%”等只解释设计动机，不是 NovaTerm 的性能承诺。

### 10.1 源码阅读顺序

1. `src/renderer/base/renderer.cpp`：调度、锁范围、行输出与重试。
2. `src/renderer/atlas/AtlasEngine.api.cpp`：失效与 setter。
3. `src/renderer/atlas/AtlasEngine.cpp` + `common.h`：帧准备、塑形与缓存数据。
4. `src/renderer/atlas/BackendD3D.cpp`、`BackendD2D.cpp`：两条实际绘制路径。
5. `src/renderer/atlas/AtlasEngine.r.cpp`：设备、交换链、Present 与资源恢复。
6. `src/cascadia/TerminalControl/ControlCore.cpp`、`TermControl.cpp`：宿主接线。

本次是本地源码静态核对，未构建或运行 Windows Terminal，未进行 GPU 性能测量。
