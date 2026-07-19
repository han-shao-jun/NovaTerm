#include "TerminalRenderer.h"
#include "core/terminal/TerminalCore.h"
#include "core/terminal/KeyMapper.h"
#include "core/terminal/ScrollbackBuffer.h"
#include <QPainter>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <algorithm>
#include <cstring>
#include <limits>

// 滚动步长（行数）
static constexpr int kScrollWheelLines = 3;
static constexpr uint32_t kWideCharContinuation = std::numeric_limits<uint32_t>::max();

TerminalRenderer::TerminalRenderer(TerminalCore* core, QWidget* parent)
    : QWidget(parent)
    , _core(core)
    , _scheme(TerminalColorScheme::defaultDark())
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);
    setAutoFillBackground(true);

    // 默认等宽字体
    _font.setFamilies({"Cascadia Code", "Consolas", "DejaVu Sans Mono", "monospace"});
    _font.setStyleHint(QFont::Monospace);
    _font.setFixedPitch(true);
    _font.setPointSize(12);
    _fm = new QFontMetricsF(_font);
    recalculateCellSize();

    // 光标闪烁定时器
    _blinkTimer = new QTimer(this);
    _blinkTimer->setInterval(530);  // ≈ 常见终端闪烁速率
    connect(_blinkTimer, &QTimer::timeout, this, [this]() {
        _cursorBlinkVisible = !_cursorBlinkVisible;
        // 只重绘光标所在行
        if (_core->cursorVisible() && _core->cursorBlink()) {
            const auto cpos = _core->cursorPosition();
            const int y = cellToWidget(cpos.row - _scrollLine, 0).y();
            update(0, y, width(), _cellHeight);
        }
    });
    _blinkTimer->start();

    // ── 连接 TerminalCore 信号 ───────────────────────────────
    connect(_core, &TerminalCore::damage, this, [this](const VTermRect& rect) {
        // libvterm 的 damage rect 在屏幕坐标中（含 scrollback 时需调整）
        const int startRow = rect.start_row - _scrollLine;
        const int endRow   = rect.end_row - _scrollLine;
        const int visRows  = _core->rows();
        if (endRow < 0 || startRow >= visRows) return;

        const int clampedStart = std::max(0, startRow);
        const int clampedEnd   = std::min(visRows, endRow);
        const int y      = clampedStart * _cellHeight;
        const int h      = (clampedEnd - clampedStart) * _cellHeight;
        const int x      = rect.start_col * _cellWidth;
        const int w      = (rect.end_col - rect.start_col) * _cellWidth;

        update(x, y, std::max(1, w), std::max(1, h));
    });

    connect(_core, &TerminalCore::cursorMoved, this, [this]() {
        // 重绘旧位置和新位置（全宽，简化处理）
        update();
    });

    connect(_core, &TerminalCore::scrollbackChanged, this, [this]() {
        update();
    });
}

// ═══════════════════════════════════════════════════════════════════
//  外观
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::setColorScheme(const TerminalColorScheme& scheme)
{
    _scheme = scheme;

    // 同步设置 libvterm 的默认颜色
    VTermColor fg, bg;
    std::memset(&fg, 0, sizeof(fg));
    std::memset(&bg, 0, sizeof(bg));
    fg.type = VTERM_COLOR_RGB;
    fg.rgb.red   = static_cast<uint8_t>(_scheme.foreground.red());
    fg.rgb.green = static_cast<uint8_t>(_scheme.foreground.green());
    fg.rgb.blue  = static_cast<uint8_t>(_scheme.foreground.blue());
    bg.type = VTERM_COLOR_RGB;
    bg.rgb.red   = static_cast<uint8_t>(_scheme.background.red());
    bg.rgb.green = static_cast<uint8_t>(_scheme.background.green());
    bg.rgb.blue  = static_cast<uint8_t>(_scheme.background.blue());

    if (_core->screen())
        vterm_screen_set_default_colors(_core->screen(), &fg, &bg);

    // 容器背景色
    QPalette pal = palette();
    pal.setColor(QPalette::Window, _scheme.background);
    setPalette(pal);

    update();
}

void TerminalRenderer::setFont(const QFont& font)
{
    _font = font;
    _font.setStyleHint(QFont::Monospace);
    _font.setFixedPitch(true);
    delete _fm;
    _fm = new QFontMetricsF(_font);
    recalculateCellSize();
    updateGeometry();
    update();
}

void TerminalRenderer::zoomIn()
{
    int sz = _font.pointSize();
    if (sz < 72) {
        _font.setPointSize(sz + 1);
        delete _fm;
        _fm = new QFontMetricsF(_font);
        recalculateCellSize();
        updateGeometry();
        update();
    }
}

void TerminalRenderer::zoomOut()
{
    int sz = _font.pointSize();
    if (sz > 4) {
        _font.setPointSize(sz - 1);
        delete _fm;
        _fm = new QFontMetricsF(_font);
        recalculateCellSize();
        updateGeometry();
        update();
    }
}

// ═══════════════════════════════════════════════════════════════════
//  滚动
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::scrollToBottom()
{
    if (_scrollLine != 0) {
        _scrollLine = 0;
        update();
    }
}

void TerminalRenderer::scrollToLine(int line)
{
    const int maxScroll = _core->scrollbackLineCount();
    const int clamped = std::max(0, std::min(line, maxScroll));
    if (clamped != _scrollLine) {
        _scrollLine = clamped;
        update();
    }
}

void TerminalRenderer::scrollLines(int delta)
{
    scrollToLine(_scrollLine + delta);
}

// ═══════════════════════════════════════════════════════════════════
//  选区
// ═══════════════════════════════════════════════════════════════════

QString TerminalRenderer::selectedText() const
{
    if (_selStart.row < 0 || _selEnd.row < 0)
        return {};

    VTermPos start = _selStart;
    VTermPos end   = _selEnd;
    if (vterm_pos_cmp(end, start) < 0)
        std::swap(start, end);

    QString result;
    for (int row = start.row; row <= end.row; ++row) {
        if (row > start.row) result += QLatin1Char('\n');

        int c1 = (row == start.row) ? start.col : 0;
        int c2 = (row == end.row)   ? end.col   : _core->columns() - 1;

        // 根据 row 判断是 scrollback 还是活跃屏幕
        for (int col = c1; col <= c2; ++col) {
            if (row < 0) {
                // Scrollback 区域
                const int sbLine = _core->scrollbackLineCount() + row;  // row 是负值
                ScrollbackCell sc;
                if (_core->getScrollbackCell(sbLine, col, sc) && sc.chars[0] &&
                    sc.chars[0] != kWideCharContinuation) {
                    result += cellCharsToString(sc.chars, VTERM_MAX_CHARS_PER_CELL);
                } else if (_core->getScrollbackCell(sbLine, col, sc) &&
                           sc.chars[0] == kWideCharContinuation) {
                    continue;
                } else {
                    result += QLatin1Char(' ');
                }
            } else {
                VTermScreenCell cell;
                if (_core->getCell(row, col, cell) && cell.chars[0] &&
                    cell.chars[0] != kWideCharContinuation) {
                    result += cellCharsToString(cell.chars, VTERM_MAX_CHARS_PER_CELL);
                } else if (_core->getCell(row, col, cell) &&
                           cell.chars[0] == kWideCharContinuation) {
                    continue;
                } else {
                    result += QLatin1Char(' ');
                }
            }
        }
    }
    return result;
}

bool TerminalRenderer::hasSelection() const
{
    return _selStart.row >= 0 && _selEnd.row >= 0 &&
           !(_selStart.row == _selEnd.row && _selStart.col == _selEnd.col);
}

void TerminalRenderer::copySelection()
{
    if (!hasSelection()) return;
    QApplication::clipboard()->setText(selectedText());
}

void TerminalRenderer::clearSelection()
{
    _selStart = {-1, -1};
    _selEnd   = {-1, -1};
    _selecting = false;
    update();
}

// ═══════════════════════════════════════════════════════════════════
//  坐标转换
// ═══════════════════════════════════════════════════════════════════

QPoint TerminalRenderer::widgetToCell(const QPoint& pos) const
{
    return QPoint(pos.x() / _cellWidth,
                  pos.y() / _cellHeight + _scrollLine);
}

// ═══════════════════════════════════════════════════════════════════
//  paintEvent
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRect dirty = event->rect();
    renderCells(p, dirty);
    renderSelection(p);
    renderCursor(p);
}

// ═══════════════════════════════════════════════════════════════════
//  resizeEvent
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);

    recalculateCellSize();

    const int cols = width()  / std::max(1, _cellWidth);
    const int rows = height() / std::max(1, _cellHeight);

    // 最小可显示行列数保护。窗口被拖到很小（但非 0）时，cols/rows 会
    // 跌到 2×1 这类病态尺寸：bash/readline 在其上重绘提示符会产生海量
    // 换行与光标移动输出，填满 PTY 缓冲并使主事件循环长时间处理
    // readyRead 而无法刷新 UI —— 表现为程序卡死。
    // 当窗口小于该阈值时，保持上一次的有效终端尺寸不变，PTY 不再收到
    // 病态尺寸；渲染时由 Qt 按可视区域自然裁剪，窗口放大后自动恢复。
    constexpr int kMinCols = 10;
    constexpr int kMinRows = 2;
    if (cols < kMinCols || rows < kMinRows)
        return;

    if (cols != _core->columns() || rows != _core->rows()) {
        _core->resize(cols, rows);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  键盘事件 → TerminalCore
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::keyPressEvent(QKeyEvent* event)
{
    emit activityDetected();

    // 滚动到行尾
    if (_scrollLine > 0)
        scrollToBottom();

    _core->processKeyPress(event);
}

// ═══════════════════════════════════════════════════════════════════
//  鼠标事件
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::mousePressEvent(QMouseEvent* event)
{
    emit activityDetected();
    setFocus();

    const QPoint cell = widgetToCell(event->pos());
    const int row = cell.y() - _scrollLine;
    const int col = cell.x();

    if (event->button() == Qt::LeftButton) {
        _selecting = true;
        _selStart  = {row, col};
        _selEnd    = {row, col};
        update();
    } else {
        // 将鼠标事件转发给 libvterm（右键/中键用于终端程序如 vim）
        const auto vmod = KeyMapper::qtModToVTermMod(event->modifiers());
        const int button = (event->button() == Qt::RightButton)  ? 2
                         : (event->button() == Qt::MiddleButton) ? 3
                         : 1;
        _core->processMousePress(event);
    }
}

void TerminalRenderer::mouseMoveEvent(QMouseEvent* event)
{
    if (_selecting) {
        const QPoint cell = widgetToCell(event->pos());
        _selEnd = {cell.y() - _scrollLine, cell.x()};
        update();
    }
}

void TerminalRenderer::mouseReleaseEvent(QMouseEvent* event)
{
    if (_selecting && event->button() == Qt::LeftButton) {
        _selecting = false;
        const QPoint cell = widgetToCell(event->pos());
        _selEnd = {cell.y() - _scrollLine, cell.x()};
        // 自动复制到剪贴板（xterm 行为）
        if (hasSelection())
            copySelection();
    } else {
        _core->processMouseRelease(event);
    }
}

void TerminalRenderer::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        // 按词选择：以空格/标点为分隔
        const QPoint cell = widgetToCell(event->pos());
        const int row = cell.y() - _scrollLine;
        const int col = cell.x();

        VTermScreenCell centerCell;
        if (_core->getCell(row, col, centerCell) && centerCell.chars[0]) {
            // 向左扩展到词边界
            int lc = col;
            while (lc > 0) {
                VTermScreenCell c;
                if (!_core->getCell(row, lc - 1, c) || c.chars[0] == 0 || c.chars[0] == ' ')
                    break;
                --lc;
            }
            // 向右扩展到词边界
            int rc = col;
            const int maxCol = _core->columns() - 1;
            while (rc < maxCol) {
                VTermScreenCell c;
                if (!_core->getCell(row, rc + 1, c) || c.chars[0] == 0 || c.chars[0] == ' ')
                    break;
                ++rc;
            }
            _selStart = {row, lc};
            _selEnd   = {row, rc};
            _selecting = false;
            copySelection();
            update();
        }
    }
}

void TerminalRenderer::wheelEvent(QWheelEvent* event)
{
    _wheelAccum += event->angleDelta().y();
    const int lines = _wheelAccum / 120 * kScrollWheelLines;  // 120 = 标准滚轮单位
    if (lines != 0) {
        _wheelAccum -= (lines / kScrollWheelLines) * 120;
        if (lines < 0) {
            scrollLines(-lines);  // 向下滚动查看更多历史
        } else {
            scrollLines(-lines);  // 向上滚动回到最新
        }
    }
}

void TerminalRenderer::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (_core->state())
        vterm_state_focus_in(_core->state());
}

void TerminalRenderer::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    if (_core->state())
        vterm_state_focus_out(_core->state());
}

// ═══════════════════════════════════════════════════════════════════
//  内部实现
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::recalculateCellSize()
{
    // 等宽字体：所有字符宽度相同
    _cellWidth  = static_cast<int>(_fm->horizontalAdvance(QLatin1Char('W')));
    if (_cellWidth < 4) _cellWidth = 8;  // 安全下限
    _cellHeight = static_cast<int>(_fm->height());
    if (_cellHeight < 4) _cellHeight = 16;
}

QPoint TerminalRenderer::cellToWidget(int row, int col) const
{
    return QPoint(col * _cellWidth, row * _cellHeight);
}

int TerminalRenderer::cellRowAt(int widgetY) const
{
    return widgetY / _cellHeight;
}

int TerminalRenderer::cellColAt(int widgetX) const
{
    return widgetX / _cellWidth;
}

// ── 渲染 ─────────────────────────────────────────────────────

void TerminalRenderer::renderCells(QPainter& p, const QRect& dirty)
{
    const int visRows    = _core->rows();
    const int cols       = _core->columns();
    const int sbCount    = _core->scrollbackLineCount();
    const int totalLines = sbCount + visRows;

    const int startWidgetRow = std::max(0, dirty.top() / _cellHeight);
    const int endWidgetRow   = std::min(visRows, (dirty.bottom() / _cellHeight) + 1);

    for (int widgetRow = startWidgetRow; widgetRow < endWidgetRow; ++widgetRow) {
        const int screenRow = widgetRow + _scrollLine;

        for (int col = 0; col < cols; ++col) {
            const int x = col * _cellWidth;
            // 跳过不在 dirty 区域的列
            if (x + _cellWidth < dirty.left() || x > dirty.right())
                continue;

            const int y = widgetRow * _cellHeight;

            if (screenRow < 0) {
                // ── Scrollback 区域 ──────────────────────────
                // screenRow 是负的：-1 表示最新的 scrollback 行
                const int sbIdx = sbCount + screenRow;  // screenRow=-1 → sbCount-1
                ScrollbackCell sc;
                if (_core->getScrollbackCell(sbIdx, col, sc)) {
                    if (sc.chars[0] == kWideCharContinuation)
                        continue;
                    renderCell(p, x, y, sc.chars, sc.width, sc.attrs, sc.fg, sc.bg);
                } else {
                    p.fillRect(x, y, _cellWidth, _cellHeight, _scheme.background);
                }
            } else {
                // ── 活跃屏幕区域 ──────────────────────────────
                VTermScreenCell cell;
                if (_core->getCell(screenRow, col, cell)) {
                    if (cell.chars[0] == kWideCharContinuation)
                        continue;
                    renderCell(p, x, y, cell.chars, cell.width,
                               cell.attrs, cell.fg, cell.bg);
                } else {
                    p.fillRect(x, y, _cellWidth, _cellHeight, _scheme.background);
                }
            }
        }
    }

    // 填充右侧和底部空白
    const int contentWidth  = cols * _cellWidth;
    const int contentHeight = visRows * _cellHeight;
    if (contentWidth < width()) {
        p.fillRect(contentWidth, 0, width() - contentWidth, height(), _scheme.background);
    }
    if (contentHeight < height()) {
        p.fillRect(0, contentHeight, width(), height() - contentHeight, _scheme.background);
    }
}

void TerminalRenderer::renderCell(QPainter& p, int x, int y,
                                   const uint32_t* chars, char width,
                                   const VTermScreenCellAttrs& attrs,
                                   const VTermColor& fg_vc, const VTermColor& bg_vc)
{
    const int cellSpan = std::max(1, static_cast<int>(width));
    const int paintWidth = _cellWidth * cellSpan;

    QColor fg = vtermColorToQColor(fg_vc);
    QColor bg = vtermColorToQColor(bg_vc);

    // Reverse: 交换前景/背景
    if (attrs.reverse) std::swap(fg, bg);

    // ── 背景 ─────────────────────────────────────────────────
    p.fillRect(x, y, paintWidth, _cellHeight, bg);

    if (attrs.conceal)
        return;  // 隐藏文字

    if (chars[0] == 0 || chars[0] == ' ')
        return;  // 空 cell，只画背景

    // ── 前景文字 ─────────────────────────────────────────────
    QFont f = _font;
    if (attrs.bold) f.setBold(true);

    p.setFont(f);
    p.setPen(fg);

    const QString text = cellCharsToString(chars, VTERM_MAX_CHARS_PER_CELL);
    if (text.isEmpty()) return;

    // 文字基线对齐
    const int textY = y + static_cast<int>(_fm->ascent());
    p.drawText(x, textY, text);

    // ── 下划线 ───────────────────────────────────────────────
    if (attrs.underline != VTERM_UNDERLINE_OFF) {
        const int ulY = y + static_cast<int>(_fm->ascent()) + 2;
        p.setPen(fg);
        if (attrs.underline == VTERM_UNDERLINE_DOUBLE) {
            p.drawLine(x, ulY, x + paintWidth, ulY);
            p.drawLine(x, ulY + 2, x + paintWidth, ulY + 2);
        } else {
            // SINGLE or CURLY (curl 暂简化为单线)
            p.drawLine(x, ulY, x + paintWidth, ulY);
        }
    }

    // ── 删除线 ───────────────────────────────────────────────
    if (attrs.strike) {
        const int stY = y + _cellHeight / 2;
        p.setPen(fg);
        p.drawLine(x, stY, x + paintWidth, stY);
    }
}

void TerminalRenderer::renderCursor(QPainter& p)
{
    if (!_core->cursorVisible() || _scrollLine != 0)
        return;

    const auto cpos = _core->cursorPosition();
    if (cpos.row < 0 || cpos.row >= _core->rows() ||
        cpos.col < 0 || cpos.col >= _core->columns())
        return;

    const QPoint widgetPos = cellToWidget(cpos.row, cpos.col);

    // 光标闪烁
    if (_core->cursorBlink() && !_cursorBlinkVisible)
        return;

    const QColor cursorColor = _scheme.cursorColor.isValid()
        ? _scheme.cursorColor
        : _scheme.foreground;

    p.save();

    // 读取光标位置的 cell 以获取前景色
    VTermScreenCell cell;
    QColor cellFg = _scheme.foreground;
    if (_core->getCell(cpos.row, cpos.col, cell) && cell.chars[0]) {
        cellFg = vtermColorToQColor(cell.fg);
    }

    switch (_core->cursorShape()) {
    case VTERM_PROP_CURSORSHAPE_UNDERLINE:
        p.fillRect(widgetPos.x(),
                    widgetPos.y() + _cellHeight - 2,
                    _cellWidth, 2, cursorColor);
        break;
    case VTERM_PROP_CURSORSHAPE_BAR_LEFT:
        p.fillRect(widgetPos.x(), widgetPos.y(),
                    2, _cellHeight, cursorColor);
        break;
    case VTERM_PROP_CURSORSHAPE_BLOCK:
    default: {
        // Block: 反转 cell 颜色
        p.setCompositionMode(QPainter::CompositionMode_Difference);
        p.fillRect(widgetPos.x(), widgetPos.y(),
                    _cellWidth, _cellHeight, Qt::white);
        break;
    }
    }

    p.restore();
}

void TerminalRenderer::renderSelection(QPainter& p)
{
    if (!hasSelection()) return;

    VTermPos start = _selStart;
    VTermPos end   = _selEnd;
    if (vterm_pos_cmp(end, start) < 0)
        std::swap(start, end);

    const QColor selColor = _scheme.selectionColor.isValid()
        ? _scheme.selectionColor
        : QColor(84, 107, 138, 128);

    for (int row = start.row; row <= end.row; ++row) {
        const int widgetRow = row - _scrollLine;
        if (widgetRow < 0 || widgetRow >= _core->rows())
            continue;

        int c1 = (row == start.row) ? start.col : 0;
        int c2 = (row == end.row)   ? end.col   : _core->columns() - 1;

        const QPoint tl = cellToWidget(widgetRow, c1);
        const QPoint br = cellToWidget(widgetRow, c2 + 1);
        p.fillRect(tl.x(), tl.y(),
                    br.x() - tl.x(), _cellHeight,
                    selColor);
    }
}

// ── 颜色转换 ─────────────────────────────────────────────────

QColor TerminalRenderer::vtermColorToQColor(const VTermColor& vc) const
{
    if (VTERM_COLOR_IS_RGB(&vc)) {
        return QColor(vc.rgb.red, vc.rgb.green, vc.rgb.blue);
    }
    if (VTERM_COLOR_IS_INDEXED(&vc)) {
        const int idx = vc.indexed.idx;
        if (idx < 16)
            return _scheme.palette[idx];
        if (idx < 232) {
            // 6x6x6 颜色立方体
            const int r = (idx - 16) / 36;
            const int g = ((idx - 16) % 36) / 6;
            const int b = (idx - 16) % 6;
            return QColor(r * 51, g * 51, b * 51);
        }
        // 灰度渐变 (232-255)
        const int gray = (idx - 232) * 10 + 8;
        return QColor(gray, gray, gray);
    }
    if (VTERM_COLOR_IS_DEFAULT_FG(&vc))
        return _scheme.foreground;
    if (VTERM_COLOR_IS_DEFAULT_BG(&vc))
        return _scheme.background;

    return _scheme.foreground;  // fallback
}

// ── Unicode → QString ───────────────────────────────────────

QString TerminalRenderer::cellCharsToString(const uint32_t* chars, int maxCount)
{
    QString result;
    for (int i = 0; i < maxCount && chars[i] != 0; ++i) {
        if (QChar::requiresSurrogates(chars[i])) {
            result += QChar::fromUcs4(chars[i]);
        } else {
            result += QChar(static_cast<ushort>(chars[i]));
        }
    }
    return result;
}
