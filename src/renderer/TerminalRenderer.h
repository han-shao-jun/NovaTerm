#pragma once
#include <QRhiWidget>
#include <QFont>
#include <QTimer>
#include <QPoint>
#include <QImage>
#include <QMutex>
#include <QHash>
#include <QRect>
#include <QVector>
#include <rhi/qshader.h>
#include <memory>
#include "core/terminal/TerminalTypes.h"
#include "core/search/SearchEngine.h"
#include "core/scrollback/LineLayout.h"
#include "RenderCommandBuffer.h"
#include "RenderScheduler.h"
#include "TerminalColorScheme.h"
#include "font/FontManager.h"
#include "glyph/GlyphCache.h"
#include "glyph/GlyphRasterizer.h"
#include "gpu/BufferBudget.h"
#include "gpu/RendererCapabilities.h"
#include "gpu/RowSlotMap.h"

class TerminalCore;
class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
namespace NovaTerm {
struct RendererSnapshot;
}
// 基于 QRhi 的终端渲染 Widget。
// 从 TerminalCore 读取活跃屏幕 cell，从 ScrollbackBuffer 读取历史行，
// 使用 GPU 批量四边形和持久字形图集绘制，CPU 仅栅格化缓存未命中的字形。
class TerminalRenderer : public QRhiWidget
{
    Q_OBJECT
public:
    struct RenderStatistics
    {
        NovaTerm::RenderScheduleStatistics scheduler;
        quint64 rowsRebuilt{0};
        quint64 commandsGenerated{0};
        quint64 commandGenerationNanoseconds{0};
        quint64 cpuFrameNanoseconds{0};
        quint64 gpuUploadBytes{0};
        quint64 contentUploadBytes{0};
        quint64 atlasUploadBytes{0};
        quint64 drawCalls{0};
        quint64 vertexBufferReallocations{0};
        quint64 revisionPromotedFullFrames{0};
        quint64 revisionRecoveredRows{0};
        quint64 lastRenderedRevision{0};
        quint64 framesRendered{0};
        quint64 cpuFramesOverBudget{0};
        quint64 cpuFrameP50Nanoseconds{0};
        quint64 cpuFrameP95Nanoseconds{0};
        quint64 cpuFrameP99Nanoseconds{0};
        quint64 dirtyBlocksRebuilt{0};
        quint64 mappingOnlyUpdates{0};
        quint64 rowSlotsReused{0};
        quint64 rowSlotsCreated{0};
        quint64 glyphCacheHits{0};
        quint64 glyphCacheMisses{0};
        quint64 glyphRasters{0};
        quint64 glyphEvictions{0};
        quint64 bufferCurrentBytes{0};
        quint64 bufferPeakBytes{0};
        quint64 atlasCurrentBytes{0};
        quint64 atlasPeakBytes{0};
        quint64 memoryCurrentBytes{0};
        quint64 memoryPeakBytes{0};
        quint64 glyphRasterQueueDepth{0};
        quint64 glyphRasterQueuePeakDepth{0};
        quint64 glyphRasterQueueRejected{0};
        quint64 glyphRasterQueueCancelled{0};
        quint64 glyphRasterQueueStaleDropped{0};
        quint64 scrollbackReflowRequests{0};
        quint64 capabilityFallbacks{0};
        quint64 viewportMappingRevision{0};
    };

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
    RenderStatistics renderStatistics() const;
    void setTargetRefreshRate(int hz);
    void setSearchMatches(QVector<NovaTerm::SearchMatch> matches,
                          quint64 generation);
    void appendSearchMatches(QVector<NovaTerm::SearchMatch> matches,
                             quint64 generation);
    void clearSearchMatches();
    qsizetype searchMatchCount() const;
    qsizetype searchMatchedLineCount() const
    {
        return _searchMatchesByLine.size();
    }

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
    struct GpuInstance
    {
        float left;
        float top;
        float right;
        float bottom;
        float u0;
        float v0;
        float u1;
        float v1;
        float r;
        float g;
        float b;
        float a;
        float atlasPage;
        float rowSlot;
        float flags;
        float reserved;
    };

    // ── 渲染辅助 ──────────────────────────────────────────────
    void recalculateCellSize();
    void resizeTerminalToViewport();
    QPoint cellToWidget(int documentRow, int col) const;
    int cellRowAt(int widgetY) const;
    int cellColAt(int widgetX) const;
    bool isDocumentPositionValid(const NovaTerm::Position& pos) const;
    uint32_t documentCellCodepoint(int documentRow, int col) const;

    bool rebuildCommandRows(const NovaTerm::RendererSnapshot& screen,
                            const QVector<bool>& dirtyRows,
                            const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
                            quint64& commandsGenerated);
    void rebuildCommandRow(int widgetRow,
                           const NovaTerm::RendererSnapshot& screen,
                           const QVector<NovaTerm::DirtyColumnSpan>& dirtySpans);
    quint64 rebuildOverlays(const NovaTerm::CursorState& cursor);
    void appendCellCommands(qreal x, qreal y, const NovaTerm::Cell& cell,
                            QVector<NovaTerm::RenderCommand>& backgrounds,
                            QVector<NovaTerm::RenderCommand>& contents);
    void appendCursorCommand(QVector<NovaTerm::RenderCommand>& commands,
                             const NovaTerm::CursorState& cursor);
    void appendSelectionCommands(QVector<NovaTerm::RenderCommand>& commands);
    void appendSearchCommands(QVector<NovaTerm::RenderCommand>& commands);
    NovaTerm::RenderCommand makeSolidCommand(
        NovaTerm::RenderCommandType type,
        const QRectF& rect,
        const QColor& color) const;
    void appendCommandVertices(const NovaTerm::RenderCommand& command,
                               const QSize& pixelSize);
    void appendSolidRect(const QRectF& rect, const QColor& color,
                         const QSize& pixelSize);
    void appendTexturedRect(const QRectF& rect, const QRect& atlasRect,
                            const QColor& color, const QSize& pixelSize);
    void appendQuad(const QRectF& rect, const QRectF& uvRect,
                    const QColor& color, const QSize& pixelSize);
    NovaTerm::GlyphLocation ensureGlyph(const QString& text, bool bold,
                                        int cellSpan);
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
    void requestFullFrame();
    void requestOverlayFrame();
    void scheduleReflow();
    bool ensureVertexBuffer(int rows, int columns);
    void updatePlacementBuffer(QRhiResourceUpdateBatch* updates,
                               const QSize& pixelSize);
    void uploadAtlasChanges(QRhiResourceUpdateBatch* updates);
    void uploadCommands(QRhiResourceUpdateBatch* updates,
                        const QSize& pixelSize,
                        const QVector<bool>& dirtyRows,
                        const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
                        bool uploadAllRows,
                        bool overlayDirty);
    void recordCpuFrame(quint64 elapsedNanoseconds);

    TerminalCore* _core;
    TerminalColorScheme _scheme;

    QFont _font;
    QFontMetricsF* _fm{nullptr};
    NovaTerm::FontManager _fontManager;
    NovaTerm::GlyphRasterizer _glyphRasterizer;
    NovaTerm::BoundedGlyphRasterQueue _glyphRasterQueue{512};
    NovaTerm::GlyphCache _glyphCache;
    NovaTerm::GlyphLocation _solidGlyph;
    // Keep the font's fractional advances.  Rounding every cell separately
    // makes the error accumulate across a line (Cascadia Mono at 16 px is
    // typically 9.6 px wide, not 10 px).
    qreal _cellWidth{0.0};
    qreal _cellHeight{0.0};

    // 滚动
    int _scrollLine{0};   // 当前滚动到 scrollback 中的行偏移（0=底部最新）
    NovaTerm::LineId _scrollAnchorLine{0};
    qsizetype _scrollAnchorWrap{0};
    quint64 _reflowGeneration{0};
    QVector<NovaTerm::DisplayLine> _historyLayout;
    QVector<NovaTerm::DisplayLine> _pendingHistoryLayout;
    quint64 _searchGeneration{0};
    QHash<NovaTerm::LineId, QVector<NovaTerm::SearchMatch>>
        _searchMatchesByLine;

    // 光标闪烁
    QTimer* _blinkTimer;
    QTimer* _reflowDebounce{nullptr};
    NovaTerm::RenderScheduler* _renderScheduler{nullptr};
    bool _cursorBlinkVisible{true};

    // 鼠标选区
    bool _selecting{false};
    NovaTerm::Position _selStart{-1, -1};
    NovaTerm::Position _selEnd{-1, -1};

    // 用于跟踪 wheel 事件累积
    int _wheelAccum{0};
    int _zoomWheelAccum{0};

    QRhi* _rhi{nullptr};
    quint64 _atlasGeneration{0};
    qreal _atlasDpr{0.0};
    quint64 _frameNumber{0};
    QVector<GpuInstance> _instances;
    NovaTerm::BufferBudget _bufferBudget;
    NovaTerm::RendererCapabilities _capabilities;
    NovaTerm::RowSlotMap _rowSlotMap;
    QVector<int> _widgetRowToSlot;
    QVector<quint64> _rowContentIdentities;
    quint64 _viewportMappingRevision{0};
    int _pendingLiveScrollRows{0};
    NovaTerm::RenderCommandBuffer _commandBuffer;
    // QRhi may consume a frame while queued terminal damage is being
    // published. Protect the hand-off and never iterate the live queue.
    QMutex _pendingFrameMutex;
    QVector<NovaTerm::DirtyRegion> _pendingDirtyRegions;
    int _backgroundRowStrideVertices{0};
    int _contentRowStrideVertices{0};
    int _overlayBaseVertex{0};
    int _overlayCapacityVertices{0};
    int _overlayVertexCount{0};
    bool _fullFramePending{true};
    bool _explicitFullPending{true};
    bool _overlayPending{true};
    quint64 _pendingContentRevision{0};
    RenderStatistics _renderStatistics;
    QVector<quint64> _cpuFrameSamples;
    qsizetype _cpuFrameSampleCursor{0};
    int _vertexBufferSize{0};
    std::unique_ptr<QRhiTexture> _atlasTexture;
    std::unique_ptr<QRhiSampler> _sampler;
    std::unique_ptr<QRhiBuffer> _vertexBuffer;
    std::unique_ptr<QRhiBuffer> _placementBuffer;
    std::unique_ptr<QRhiShaderResourceBindings> _srb;
    std::unique_ptr<QRhiGraphicsPipeline> _pipeline;
};
