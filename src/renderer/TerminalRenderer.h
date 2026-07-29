#pragma once
#include <QRhiWidget>
#include <QFont>
#include <QTimer>
#include <QPoint>
#include <QImage>
#include <QHash>
#include <QRect>
#include <QVector>
#include <rhi/qshader.h>
#include <memory>
#include "core/terminal/TerminalTypes.h"
#include "TerminalColorScheme.h"

class TerminalCore;
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
// 基于 QRhi 的终端渲染 Widget。
// 从 TerminalCore 读取活跃屏幕 cell，从 ScrollbackBuffer 读取历史行，
// 使用 GPU 批量四边形和持久字形图集绘制，CPU 仅栅格化缓存未命中的字形。
class TerminalRenderer : public QRhiWidget
{
    Q_OBJECT
public:
    explicit TerminalRenderer(TerminalCore* core, QWidget* parent = nullptr);
    ~TerminalRenderer() override;

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
    void terminalSizeChanged();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    struct GpuVertex
    {
        float x;
        float y;
        float u;
        float v;
        float r;
        float g;
        float b;
        float a;
    };

    struct GlyphEntry
    {
        QRect pixelRect;
        QRectF logicalRect;
    };

    // ── 渲染辅助 ──────────────────────────────────────────────
    void recalculateCellSize();
    void resizeTerminalToViewport();
    QPoint cellToWidget(int documentRow, int col) const;
    int cellRowAt(int widgetY) const;
    int cellColAt(int widgetX) const;
    bool isDocumentPositionValid(const NovaTerm::Position& pos) const;
    uint32_t documentCellCodepoint(int documentRow, int col) const;

    void buildGpuFrame(const QSize& pixelSize);
    void appendCell(qreal x, qreal y, const NovaTerm::Cell& cell,
                    const QSize& pixelSize, bool backgroundPass);
    void appendCursor(const QSize& pixelSize);
    void appendSelection(const QSize& pixelSize);
    void appendSolidRect(const QRectF& rect, const QColor& color,
                         const QSize& pixelSize);
    void appendTexturedRect(const QRectF& rect, const QRect& atlasRect,
                            const QColor& color, const QSize& pixelSize);
    void appendQuad(const QRectF& rect, const QRectF& uvRect,
                    const QColor& color, const QSize& pixelSize);
    const GlyphEntry& ensureGlyph(const QString& text, bool bold, int cellSpan);
    void resetGlyphAtlas();


    // ── 颜色转换 ──────────────────────────────────────────────
    QColor terminalColorToQColor(const NovaTerm::TerminalColor& color,
                                 bool foreground) const;

    // ── Unicode 转 UTF-8 ──────────────────────────────────────
    static QString cellCharsToString(const uint32_t* chars, int maxCount);
    static QShader loadShader(const QString& path);
    void releaseRhiResources();
    void ensureAtlasTexture();
    void ensurePipeline();

    TerminalCore* _core;
    TerminalColorScheme _scheme;

    QFont _font;
    QFontMetricsF* _fm{nullptr};
    // Keep the font's fractional advances.  Rounding every cell separately
    // makes the error accumulate across a line (Cascadia Mono at 16 px is
    // typically 9.6 px wide, not 10 px).
    qreal _cellWidth{0.0};
    qreal _cellHeight{0.0};

    // 滚动
    int _scrollLine{0};   // 当前滚动到 scrollback 中的行偏移（0=底部最新）

    // 光标闪烁
    QTimer* _blinkTimer;
    bool _cursorBlinkVisible{true};

    // 鼠标选区
    bool _selecting{false};
    NovaTerm::Position _selStart{-1, -1};
    NovaTerm::Position _selEnd{-1, -1};

    // 用于跟踪 wheel 事件累积
    int _wheelAccum{0};
    int _zoomWheelAccum{0};

    QRhi* _rhi{nullptr};
    QImage _atlasImage;
    QHash<QString, GlyphEntry> _glyphs;
    int _atlasX{1};
    int _atlasY{1};
    int _atlasRowHeight{0};
    bool _atlasDirty{true};
    qreal _atlasDpr{0.0};
    QVector<GpuVertex> _vertices;
    int _vertexBufferSize{0};
    std::unique_ptr<QRhiTexture> _atlasTexture;
    std::unique_ptr<QRhiSampler> _sampler;
    std::unique_ptr<QRhiBuffer> _vertexBuffer;
    std::unique_ptr<QRhiShaderResourceBindings> _srb;
    std::unique_ptr<QRhiGraphicsPipeline> _pipeline;
};
