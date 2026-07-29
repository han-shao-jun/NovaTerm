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

## 2.2 Glyph Atlas 使用 boundingRect 导致宽度偏差

当前逻辑：

``` cpp
const QRectF inkRect =
    glyphMetrics.boundingRect(text);
```

`boundingRect()` 返回的是字符视觉包围盒。

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

## 3.3 修改 Glyph Atlas 尺寸计算

不要使用：

``` cpp
boundingRect()
```

替换：

``` cpp
const qreal advance =
    glyphMetrics.horizontalAdvance(text);

const QRectF inkRect(
    0,
    -glyphMetrics.ascent(),
    advance,
    glyphMetrics.height()
);
```

这样：

    Cell Width
          =
    Glyph Advance

保证终端栅格一致。

------------------------------------------------------------------------

## 3.4 修正 Glyph 绘制基线

原代码：

``` cpp
painter.drawText(
    QPointF(
        -logicalRect.left(),
        baseline - logicalRect.top()
    ),
    text
);
```

建议：

``` cpp
painter.drawText(
    QPointF(
        -logicalRect.left(),
        glyphMetrics.ascent()
    ),
    text
);
```

避免 glyph 在 atlas 中产生额外偏移。

------------------------------------------------------------------------

# 4. DPI 适配建议

NovaTerm 使用 QRhi + Glyph Atlas，需要考虑高 DPI。

建议：

Atlas：

    logical size
            |
            x devicePixelRatio
            |
    physical texture

例如：

125% DPI：

    2048 x 1.25

150% DPI：

    2048 x 1.5

避免：

-   字符模糊
-   边缘锯齿
-   缩放失真

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
