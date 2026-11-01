# NovaTerm TerminalRenderer 字符间距异常修复指南

## 1. 问题现象

NovaTerm 终端界面字符显示异常：

-   英文字符之间出现明显空隙：

```{=html}
<!-- -->
```
    W i n d o w s   P o w e r S h e l l

-   中文字符也出现拉伸：

```{=html}
<!-- -->
```
    版 权 所 有 （C） M i c r o s o f t

而 Windows Terminal 显示正常：

    Windows PowerShell
    版权所有（C） Microsoft Corporation

现象说明：

-   Qt 应用整体字体没有问题；
-   FluentUI / 关于页面文字正常；
-   问题集中在 TerminalRenderer 的字符栅格布局和 Glyph 渲染。

------------------------------------------------------------------------

# 2. 根因分析

## 2.1 TerminalRenderer 使用了应用 UI 字体

当前代码：

``` cpp
_font = qApp->font();
```

问题：

NovaTerm 主界面使用 FluentUI/ElaWidgetTools 风格字体，通常是：

-   Microsoft YaHei UI
-   Segoe UI
-   系统 UI 字体

这些字体属于比例字体（Proportional Font）。

终端需要：

-   等宽字体（Monospace Font）
-   固定 Cell 宽度
-   字符 advance 与 cell width 一致

------------------------------------------------------------------------

## 2.2 将墨迹宽度用作网格步进导致偏差

历史问题示例：

``` cpp
const QRectF inkRect =
    glyphMetrics.boundingRect(text);
```

`boundingRect()` 返回的是字符视觉包围盒。错误在于把它当作网格步进；字形位图仍需要墨迹边界以防裁剪，当前规则见3.3节。

例如：

    字符:
    W

    advance:
    10px

    ink width:
    13px

Terminal Renderer 需要：

    cell width == glyph advance

而不是：

    cell width == glyph ink width

否则：

    +---------+
    | W       |
    +---------+

          +---------+
          | i       |
          +---------+

字符会被逐渐撑开。

------------------------------------------------------------------------

# 3. 修复方案

## 3.1 TerminalRenderer 使用独立终端字体

不要继承 QApplication 字体。

修改：

``` cpp
_font = qApp->font();
```

改为：

Windows：

``` cpp
_font = QFont("Cascadia Mono");
_font.setStyleHint(QFont::Monospace);
_font.setPixelSize(16);
```

备选：

    Consolas
    JetBrains Mono

Linux：

    DejaVu Sans Mono
    Noto Sans Mono

------------------------------------------------------------------------

## 3.2 修改 Cell Width 计算

当前：

``` cpp
_cellWidth =
    qCeil(_fm->horizontalAdvance('0'));
```

建议：

``` cpp
_cellWidth =
    qCeil(_fm->horizontalAdvance('M'));
```

原因：

终端字体通常使用 M 字符作为标准宽度。

------------------------------------------------------------------------

## 3.3 字形位图尺寸与网格步进分开计算（0.2.128）

单元格宽度仍由主字体的 `horizontalAdvance('M')` 决定；回退字体不反向改变列步进。字形位图需要包含 `tightBoundingRect(text)` 的真实墨迹，包括斜体的负 bearing、右侧伸出和组合字符重音。

位图边界取网格范围、advance及墨迹范围的并集，在物理像素网格上向外取整。不要将墨迹宽度直接用作单元格宽度，也不要仅按advance裁剪位图。非有限几何及超过256KiB的位图在整数转换/分配前拒绝。

## 3.4 统一基线并保留字形边界

行基线来自终端主字体的ascent，作为 `GlyphKey.cellBaseline1024` 随不可变栅格化任务传入，并参与缓存键比较和哈希。各字体在同一行使用同一个目标基线。

设位图在单元格坐标内的原点为 `origin`，绘制位置为：

```cpp
painter.drawText(QPointF(-origin.x(), cellBaseline - origin.y()), text);
```

GPU四边形使用包含该origin的logicalRect。两者满足：`logicalRect.top() + bitmap.baseline == cellBaseline`。不能给每个回退字体各用自己的ascent，也不能再减去整个ascent而把字形移到上一行。允许墨迹超出单元格的局部原点；字体天然的字面上下边缘不必完全等高。

普通、粗体、斜体和粗斜体通过Cell属性进入字体选择与缓存；实际实现见[GlyphRasterizer](../src/renderer/glyph/GlyphRasterizer.cpp)。没有按字体名称或平台硬编码补偿值。

# 4. DPI规则与验证边界

位图尺寸与原点在物理像素边界取整，再用同一scale还原为logicalRect；atlas页仍遵守其物理尺寸及内存预算，不再额外乘一次DPR。点字号绘制时，QImage的逻辑字体DPI与QFontMetricsF::fontDpi一致，避免平台默认图像DPI和字体度量DPI不同。网格度量仍只在字体/布局变化时计算，不在GUI每个字符上重复测量。

0.2.128使用系统FixedFont/GeneralFont测试两类字体、点/像素字号、回退字体高/低两方向、普通/粗体/斜体、组合符和已给定Emoji cluster的墨迹边界。实际字体DPI72/96/120/144/192与位图DPR1/1.25/1.5/1.75/2/3分别验证。矩阵测试不代表对应物理显示器实测；复杂Emoji的跨Cell聚类与系统字体覆盖也不由这次基线修复保证。

Linux OpenGL/Vulkan及当前约1.823倍DPR截图已验证。Windows D3D与macOS Metal、真实跨屏移动尚未实机验证；实现使用通用Qt字体度量与QRhi坐标，不包含本机字体特例。

------------------------------------------------------------------------

# 5. 推荐代码调整顺序

## 第一阶段（立即修复）

修改：

1.  TerminalRenderer 独立字体

``` cpp
Cascadia Mono
```

2.  glyphMetrics 使用 horizontalAdvance

3.  cellWidth 使用等宽字体宽度

完成后：

显示应该从：

    W i n d o w s   P o w e r S h e l l

恢复：

    Windows PowerShell

------------------------------------------------------------------------

## 第二阶段（架构优化）

增加：

    TerminalFontManager
            |
            +-- QFont
            |
            +-- FontMetrics
            |
            +-- Cell Size
            |
            +-- Glyph Cache
            |
            +-- Glyph Atlas

TerminalRenderer 不直接管理字体。

------------------------------------------------------------------------

# 6. 推荐 NovaTerm 终端渲染架构

    TerminalCore
        |
        |  libvterm
        |
    TerminalFontManager
        |
        |  glyph metrics
        |  atlas cache
        |
    TerminalRenderer
        |
        | QRhi
        |
    GPU

优势：

后续支持：

-   字体切换
-   Ctrl + 鼠标滚轮缩放
-   Nerd Font
-   中文 fallback
-   Unicode 宽字符
-   ligature

------------------------------------------------------------------------

# 7. 不建议的方案

不要：

-   使用 QML Text 组件绘制终端字符
-   使用大量 QLabel/QTextEdit 模拟终端
-   回退到全量 QPainter 绘制

NovaTerm 的目标是：

    百万字符输出
    GPU 加速
    高性能终端模拟

当前 QRhi + Glyph Atlas 路线正确。

------------------------------------------------------------------------

# 8. 最终结论

NovaTerm 当前字符异常的主要原因：

1.  TerminalRenderer 继承了 UI 字体；
2.  Glyph Atlas 使用视觉宽度而不是终端 advance 宽度；
3.  字符 cell 布局与 glyph 实际尺寸不一致。

修复方向：

    独立终端字体
            +
    正确 glyph advance
            +
    统一 cell metrics
            +
    QRhi Glyph Atlas

即可达到 Windows Terminal 类似的字符显示效果。
