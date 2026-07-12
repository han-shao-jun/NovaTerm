#pragma once
#include <QWidget>
#include <QFont>
#include <QTimer>
#include <QPoint>
#include <vterm.h>
#include "TerminalColorScheme.h"

class TerminalCore;
class ScrollbackCell;

// 基于 QPainter 的终端渲染 Widget。
// 从 TerminalCore 读取活跃屏幕 cell，从 ScrollbackBuffer 读取历史行，
// 处理光标闪烁、鼠标选区和字体缩放。
class TerminalRenderer : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalRenderer(TerminalCore* core, QWidget* parent = nullptr);

    // ── 外观 ───────────────────────────────────────────────────
    void setColorScheme(const TerminalColorScheme& scheme);
    const TerminalColorScheme& colorScheme() const { return _scheme; }

    void setFont(const QFont& font);
    QFont font() const { return _font; }

    void zoomIn();
    void zoomOut();

    // ── 滚动 ───────────────────────────────────────────────────
    int scrollOffset() const { return _scrollLine; }
    void scrollToBottom();
    void scrollToLine(int line);
    void scrollLines(int delta);

    // ── 选区 ───────────────────────────────────────────────────
    QString selectedText() const;
    bool hasSelection() const;
    void copySelection();
    void clearSelection();

    // ── 从 widget 坐标计算 cell 坐标（供外部使用）─────────────
    QPoint widgetToCell(const QPoint& pos) const;

signals:
    void activityDetected();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    // ── 渲染辅助 ──────────────────────────────────────────────
    void recalculateCellSize();
    QPoint cellToWidget(int row, int col) const;
    int cellRowAt(int widgetY) const;
    int cellColAt(int widgetX) const;

    void renderCells(QPainter& p, const QRect& rect);
    void renderCell(QPainter& p, int x, int y,
                    const uint32_t* chars, char width,
                    const VTermScreenCellAttrs& attrs,
                    const VTermColor& fg, const VTermColor& bg);
    void renderCursor(QPainter& p);
    void renderSelection(QPainter& p);

    // ── 颜色转换 ──────────────────────────────────────────────
    QColor vtermColorToQColor(const VTermColor& vc) const;

    // ── Unicode 转 UTF-8 ──────────────────────────────────────
    static QString cellCharsToString(const uint32_t* chars, int maxCount);

    TerminalCore* _core;
    TerminalColorScheme _scheme;

    QFont _font;
    QFontMetricsF* _fm{nullptr};
    int _cellWidth{0};
    int _cellHeight{0};

    // 滚动
    int _scrollLine{0};   // 当前滚动到 scrollback 中的行偏移（0=底部最新）

    // 光标闪烁
    QTimer* _blinkTimer;
    bool _cursorBlinkVisible{true};

    // 鼠标选区
    bool _selecting{false};
    VTermPos _selStart{-1, -1};
    VTermPos _selEnd{-1, -1};

    // 用于跟踪 wheel 事件累积
    int _wheelAccum{0};
};
