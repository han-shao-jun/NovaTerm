#include "TerminalRenderer.h"
#include "core/terminal/TerminalCore.h"
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QMatrix4x4>
#include <QMutexLocker>
#include <rhi/qshader.h>
#include <rhi/qrhi.h>
#include <algorithm>
#include <limits>
#include <utility>

// 滚动步长（行数）
static constexpr int kScrollWheelLines = 3;
static constexpr int kMinTerminalFontSize = 8;
static constexpr int kMaxTerminalFontSize = 32;
// terminal_texture.vert uses a fixed-size std140 placement array. Keep CPU
// bounds in one named constant and make the shader reject slots outside it.
static constexpr int kMaxGpuPlacementRows = 256;
// PlacementBlock layout: viewport(4) + clipSpaceCorr mat4(16) + rowPlacement[256].
static constexpr int kPlacementMatrixOffset = 4;
static constexpr int kPlacementRowOffset = kPlacementMatrixOffset + 16;
static constexpr int kPlacementFloatCount =
    kPlacementRowOffset + 4 * (kMaxGpuPlacementRows + 1);
static constexpr qsizetype kCpuFrameSampleCapacity = 2048;
static constexpr quint64 kCpuFrameBudgetNanoseconds = 16666667;

static QImage alphaCoverageFromRgb(const QImage& rgbImage, qreal dpr)
{
    QImage coverage(rgbImage.size(), QImage::Format_RGBA8888);
    coverage.setDevicePixelRatio(dpr);

    // An opaque RGB paint device allows the Windows font engine to retain its
    // per-channel sub-pixel coverage. The terminal shader needs one portable
    // coverage value, so convert the RGB samples to luminance without applying
    // a second sharpening or thresholding pass.
    for (int y = 0; y < rgbImage.height(); ++y) {
        const QRgb* source = reinterpret_cast<const QRgb*>(rgbImage.constScanLine(y));
        uchar* destination = coverage.scanLine(y);
        for (int x = 0; x < rgbImage.width(); ++x) {
            const int alpha = qGray(source[x]);
            destination[x * 4 + 0] = 255;
            destination[x * 4 + 1] = 255;
            destination[x * 4 + 2] = 255;
            destination[x * 4 + 3] = static_cast<uchar>(alpha);
        }
    }
    return coverage;
}

static QRhiWidget::Api preferredRhiApi()
{
    const QByteArray api = qgetenv("NOVATERM_RHI_API").trimmed().toLower();
    if (api == "d3d11" || api == "direct3d11")
        return QRhiWidget::Api::Direct3D11;
    if (api == "d3d12" || api == "direct3d12")
        return QRhiWidget::Api::Direct3D12;
    if (api == "opengl" || api == "gl")
        return QRhiWidget::Api::OpenGL;
    if (api == "vulkan")
        return QRhiWidget::Api::Vulkan;
    if (api == "null")
        return QRhiWidget::Api::Null;

#ifdef Q_OS_WIN
    return QRhiWidget::Api::Direct3D11;
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
    return QRhiWidget::Api::Metal;
#else
    return QRhiWidget::Api::OpenGL;
#endif
}

static const char* rhiApiName(QRhiWidget::Api api)
{
    switch (api) {
    case QRhiWidget::Api::Direct3D11: return "Direct3D 11";
    case QRhiWidget::Api::Direct3D12: return "Direct3D 12";
    case QRhiWidget::Api::Vulkan: return "Vulkan";
    case QRhiWidget::Api::Metal: return "Metal";
    case QRhiWidget::Api::OpenGL: return "OpenGL";
    case QRhiWidget::Api::Null: return "Null";
    }
    return "Unknown";
}

TerminalRenderer::TerminalRenderer(TerminalCore* core, QWidget* parent)
    // QRhiWidget::setApi() must run before the widget is inserted into a
    // widget hierarchy. Passing parent to the base constructor would attach
    // the widget before the constructor body gets a chance to select an API,
    // potentially leaving the top-level window and this widget with different
    // QRhi backends.
    : QRhiWidget(nullptr)
    , _core(core)
    , _scheme(TerminalColorScheme::defaultDark())
{
    setApi(preferredRhiApi());

    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setCursor(Qt::IBeamCursor);
    setAutoFillBackground(true);

    // The application UI font is proportional and must not be used for a
    // terminal grid. Keep a dedicated monospace font whose advance matches the
    // fixed cell width.
#ifdef Q_OS_WIN
    _font.setFamilies({QStringLiteral("Cascadia Mono"),
                       QStringLiteral("Consolas"),
                       QStringLiteral("JetBrains Mono")});
#elif defined(Q_OS_MACOS)
    _font.setFamilies({QStringLiteral("Menlo"),
                       QStringLiteral("Monaco")});
#else
    _font.setFamilies({QStringLiteral("DejaVu Sans Mono"),
                       QStringLiteral("Noto Sans Mono"),
                       QStringLiteral("monospace")});
#endif
    _font.setStyleHint(QFont::Monospace);
    _font.setFixedPitch(true);
    _font.setPixelSize(16);
    _fontManager.setPrimaryFont(_font);
#ifdef Q_OS_LINUX
    _fontManager.setFallbackFamilies({QStringLiteral("Noto Sans Mono CJK SC"),
                                      QStringLiteral("Noto Color Emoji"),
                                      QStringLiteral("DejaVu Sans")});
#endif
    _fm = new QFontMetricsF(_font);
    recalculateCellSize();
    resetGlyphAtlas();
    _cpuFrameSamples.reserve(kCpuFrameSampleCapacity);
    _renderScheduler = new NovaTerm::RenderScheduler(this);
    _renderScheduler->setViewport(_core->columns(), _core->rows());
    connect(_renderScheduler, &NovaTerm::RenderScheduler::frameRequested,
            this,
            [this](const QVector<NovaTerm::DirtyRegion>& regions,
                   bool fullFrame,
                   bool overlayDirty,
                   quint64 contentRevision) {
        const QMutexLocker lock(&_pendingFrameMutex);
        if (fullFrame) {
            _pendingDirtyRegions.clear();
            _fullFramePending = true;
        } else if (!_fullFramePending) {
            _pendingDirtyRegions += regions;
        }
        // screenScrolled is emitted immediately before the damage regions from
        // the same parser publication. Stage its row count until the scheduler
        // hands off a content frame, so an unrelated overlay frame cannot
        // rotate the row mapping without the corresponding damage/revision.
        if (fullFrame || !regions.isEmpty())
            _scrollDamageHandoff.publish();
        _overlayPending = _overlayPending || overlayDirty;
        _pendingContentRevision = std::max(_pendingContentRevision,
                                           contentRevision);
        update();
    });

    // 光标闪烁定时器
    _blinkTimer = new QTimer(this);
    _blinkTimer->setInterval(530);  // ≈ 常见终端闪烁速率
    connect(_blinkTimer, &QTimer::timeout, this, [this]() {
        _cursorBlinkVisible = !_cursorBlinkVisible;
        // 只重绘光标所在行
        if (_core->cursorVisible() && _core->cursorBlink() && _scrollLine == 0) {
            requestOverlayFrame();
        }
    });
    _blinkTimer->start();

    _reflowDebounce = new QTimer(this);
    _reflowDebounce->setSingleShot(true);
    _reflowDebounce->setInterval(24);
    connect(_reflowDebounce, &QTimer::timeout,
            this, &TerminalRenderer::scheduleReflow);

    // ── 连接 TerminalCore 信号 ───────────────────────────────
    connect(_core, &TerminalCore::damage, this,
            [this](const NovaTerm::DirtyRegion& region, quint64 revision) {
        _renderScheduler->setViewport(_core->columns(), _core->rows());
        NovaTerm::DirtyRegion visible = region;
        visible.startRow += _scrollLine;
        visible.endRow += _scrollLine;
        _renderScheduler->schedule(visible, revision);
    });

    connect(_core, &TerminalCore::cursorMoved, this, [this]() {
        requestOverlayFrame();
    });

    connect(_core, &TerminalCore::scrollbackChanged, this, [this]() {
        const int historyCount = _historyLayout.isEmpty()
            ? _core->scrollbackLineCount() : _historyLayout.size();
        int clampedOffset = std::clamp(_scrollLine, 0, historyCount);
        const bool viewportMappingChanged =
            _scrollLine > 0 || clampedOffset != _scrollLine;
        if (clampedOffset != _scrollLine)
            _scrollLine = clampedOffset;

        bool selectionChanged = false;
        const bool hasSelectionState =
            _selecting || _selStart.col >= 0 || _selEnd.col >= 0;
        if (hasSelectionState
            && (!isDocumentPositionValid(_selStart)
                || !isDocumentPositionValid(_selEnd))) {
            _selStart = {-1, -1};
            _selEnd = {-1, -1};
            _selecting = false;
            selectionChanged = true;
        }

        // At the live bottom, a scrollback append is accompanied by damage for
        // the active screen in the same parser publication. Scheduling a full
        // frame here would turn every output batch (especially shell/Clink
        // startup) into a complete CPU rebuild and GPU upload. A full rebuild
        // is only required while history rows are actually mapped into the
        // viewport or when clamping changed that mapping.
        if (viewportMappingChanged) {
            ++_viewportMappingRevision;
            requestFullFrame();
        } else {
            if (selectionChanged)
                requestOverlayFrame();
        }
        if (_scrollLine > 0) {
            _reflowDebounce->start();
        } else {
            // Historical layout is not part of the live-bottom viewport.
            // Reflowing an ever-growing scrollback here can eventually steal
            // enough CPU to make the renderer miss damage publications and
            // promote otherwise incremental scrolls to full-row recovery.
            // Invalidate the cached layout and rebuild it lazily when the
            // user actually enters history.
            discardHistoryLayout();
        }
    });

    connect(_core, &TerminalCore::screenScrolled, this, [this](int rows) {
        if (rows <= 0)
            return;
        if (_scrollLine == 0) {
            const QMutexLocker lock(&_pendingFrameMutex);
            _scrollDamageHandoff.queue(rows);
            ++_viewportMappingRevision;
        } else {
            // History is mapped into the viewport, so row identities rather
            // than the live-screen ring determine placement.
            requestFullFrame();
        }
    });

    connect(_core, &TerminalCore::reflowBatchReady, this,
            [this](const NovaTerm::ReflowBatch& batch) {
        if (batch.generation != _reflowGeneration)
            return;
        if (batch.logicalStart == 0)
            _pendingHistoryLayout.clear();
        _pendingHistoryLayout += batch.rows;
        if (!batch.completed || !batch.error.isEmpty())
            return;
        _historyLayout = std::move(_pendingHistoryLayout);
        _scrollLine = std::clamp(_scrollLine, 0,
                                 int(_historyLayout.size()));
        if (_scrollLine > 0 && _scrollAnchorLine != 0) {
            for (qsizetype i = 0; i < _historyLayout.size(); ++i) {
                const auto& row = _historyLayout[i];
                if (row.lineId == _scrollAnchorLine
                    && row.wrapIndex == _scrollAnchorWrap) {
                    _scrollLine = int(_historyLayout.size() - i);
                    break;
                }
            }
        }
        // Reflow changes only historical mapping. At the live bottom the
        // active screen identity and placement are unchanged, so rebuilding
        // base content would violate the mapping-only contract.
        if (_scrollLine > 0) {
            ++_viewportMappingRevision;
            requestFullFrame();
        }
    });

    connect(this, &QRhiWidget::renderFailed, this, []() {
        qWarning() << "TerminalRenderer: QRhi render failed. Set NOVATERM_RHI_API=d3d11, d3d12, vulkan, or opengl to try another backend.";
    });

    // The parent may already belong to a visible top-level window, and
    // setParent() can synchronously deliver polish/layout events. Attach only
    // after every renderer member and connection has been initialized.
    if (parent)
        setParent(parent);
}

TerminalRenderer::~TerminalRenderer()
{
    _glyphRasterQueue.stop();
    if (_blinkTimer) {
        _blinkTimer->stop();
    }
    releaseRhiResources();
    // 不需要手动 disconnect _core：Qt 会在任意一方析构时自动断开所有连接。
    // 在 deleteChildren() 过程中 _core 可能已先析构，此时 disconnect 会访问
    // 半析构的 QObject 内部元数据导致 SIGSEGV。
    delete _fm;
}

TerminalRenderer::RenderStatistics TerminalRenderer::renderStatistics() const
{
    RenderStatistics result = _renderStatistics;
    if (_renderScheduler)
        result.scheduler = _renderScheduler->statistics();
    if (!_cpuFrameSamples.isEmpty()) {
        QVector<quint64> sorted = _cpuFrameSamples;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&sorted](double value) {
            const qsizetype index = std::min<qsizetype>(
                sorted.size() - 1,
                qsizetype(value * double(sorted.size() - 1)));
            return sorted[index];
        };
        result.cpuFrameP50Nanoseconds = percentile(0.50);
        result.cpuFrameP95Nanoseconds = percentile(0.95);
        result.cpuFrameP99Nanoseconds = percentile(0.99);
    }
    return result;
}

TerminalRenderer::RenderProgress TerminalRenderer::renderProgress() const
{
    return {_renderStatistics.rowsRebuilt,
            _renderStatistics.lastRenderedRevision,
            _renderStatistics.framesRendered};
}

void TerminalRenderer::setTargetRefreshRate(int hz)
{
    if (_renderScheduler)
        _renderScheduler->setTargetRefreshRate(hz);
}

// ═══════════════════════════════════════════════════════════════════
//  外观
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::setColorScheme(const TerminalColorScheme& scheme)
{
    _scheme = scheme;

    NovaTerm::TerminalColor foreground;
    foreground.type = NovaTerm::ColorType::Rgb;
    foreground.red = static_cast<uint8_t>(_scheme.foreground.red());
    foreground.green = static_cast<uint8_t>(_scheme.foreground.green());
    foreground.blue = static_cast<uint8_t>(_scheme.foreground.blue());
    NovaTerm::TerminalColor background;
    background.type = NovaTerm::ColorType::Rgb;
    background.red = static_cast<uint8_t>(_scheme.background.red());
    background.green = static_cast<uint8_t>(_scheme.background.green());
    background.blue = static_cast<uint8_t>(_scheme.background.blue());
    _core->setDefaultColors(foreground, background);

    // 容器背景色
    QPalette pal = palette();
    pal.setColor(QPalette::Window, _scheme.background);
    setPalette(pal);

    requestFullFrame();
}

void TerminalRenderer::setHighlightRules(
    QVector<NovaTerm::TerminalHighlightRule> rules)
{
    _highlightRules = std::move(rules);
    requestFullFrame();
}

void TerminalRenderer::setFont(const QFont& font)
{
    _font = font;
    if (_font.pixelSize() > 0) {
        _font.setPixelSize(std::clamp(_font.pixelSize(),
                                      kMinTerminalFontSize,
                                      kMaxTerminalFontSize));
    } else if (_font.pointSize() > 0) {
        _font.setPointSize(std::clamp(_font.pointSize(),
                                      kMinTerminalFontSize,
                                      kMaxTerminalFontSize));
    }
    delete _fm;
    _fm = new QFontMetricsF(_font);
    _fontManager.setPrimaryFont(_font);
    recalculateCellSize();
    resetGlyphAtlas();
    updateGeometry();
    resizeTerminalToViewport();
    requestFullFrame();
}

void TerminalRenderer::zoomIn()
{
    const bool usesPixelSize = _font.pixelSize() > 0;
    const int sz = usesPixelSize ? _font.pixelSize() : _font.pointSize();
    if (sz > 0 && sz < kMaxTerminalFontSize) {
        if (usesPixelSize)
            _font.setPixelSize(sz + 1);
        else
            _font.setPointSize(sz + 1);
        delete _fm;
        _fm = new QFontMetricsF(_font);
        _fontManager.setPrimaryFont(_font);
        recalculateCellSize();
        resetGlyphAtlas();
        updateGeometry();
        resizeTerminalToViewport();
        requestFullFrame();
    }
}

void TerminalRenderer::zoomOut()
{
    const bool usesPixelSize = _font.pixelSize() > 0;
    const int sz = usesPixelSize ? _font.pixelSize() : _font.pointSize();
    if (sz > kMinTerminalFontSize) {
        if (usesPixelSize)
            _font.setPixelSize(sz - 1);
        else
            _font.setPointSize(sz - 1);
        delete _fm;
        _fm = new QFontMetricsF(_font);
        _fontManager.setPrimaryFont(_font);
        recalculateCellSize();
        resetGlyphAtlas();
        updateGeometry();
        resizeTerminalToViewport();
        requestFullFrame();
    }
}

// ═══════════════════════════════════════════════════════════════════
//  滚动
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::scrollToBottom()
{
    if (_scrollLine != 0) {
        _scrollLine = 0;
        _scrollAnchorLine = 0;
        _scrollAnchorWrap = 0;
        ++_viewportMappingRevision;
        requestFullFrame();
    }
    discardHistoryLayout();
}

void TerminalRenderer::scrollToLine(int line)
{
    const int maxScroll = _historyLayout.isEmpty()
        ? _core->scrollbackLineCount() : _historyLayout.size();
    const int clamped = std::max(0, std::min(line, maxScroll));
    if (clamped != _scrollLine) {
        _scrollLine = clamped;
        ++_viewportMappingRevision;
        if (_scrollLine > 0 && !_historyLayout.isEmpty()) {
            const qsizetype row = std::max<qsizetype>(
                0, _historyLayout.size() - _scrollLine);
            _scrollAnchorLine = _historyLayout[row].lineId;
            _scrollAnchorWrap = _historyLayout[row].wrapIndex;
        } else if (_scrollLine > 0) {
            const auto history = _core->scrollbackSnapshot();
            const auto* logical = history.lineAt(std::max<qsizetype>(
                0, history.lineCount() - _scrollLine));
            _scrollAnchorLine = logical ? logical->id : history.firstLineId();
            _scrollAnchorWrap = 0;
        } else {
            _scrollAnchorLine = 0;
            _scrollAnchorWrap = 0;
        }
        requestFullFrame();
        if (_scrollLine > 0)
            _reflowDebounce->start();
        else
            discardHistoryLayout();
    }
}

void TerminalRenderer::scrollLines(int delta)
{
    scrollToLine(_scrollLine + delta);
}

void TerminalRenderer::setConservativeLiveScrollRendering(bool enabled)
{
    if (_conservativeLiveScrollRendering == enabled)
        return;
    _conservativeLiveScrollRendering = enabled;
    requestFullFrame();
}

// ═══════════════════════════════════════════════════════════════════
//  选区
// ═══════════════════════════════════════════════════════════════════

QString TerminalRenderer::selectedText() const
{
    if (!isDocumentPositionValid(_selStart) ||
        !isDocumentPositionValid(_selEnd)) {
        return {};
    }

    NovaTerm::Position start = _selStart;
    NovaTerm::Position end = _selEnd;
    if (end < start)
        std::swap(start, end);

    QString result;
    for (int row = start.row; row <= end.row; ++row) {
        if (row > start.row) result += QLatin1Char('\n');

        int c1 = (row == start.row) ? start.col : 0;
        int c2 = (row == end.row)   ? end.col   : _core->columns() - 1;

        // 根据 row 判断是 scrollback 还是活跃屏幕
        for (int col = c1; col <= c2; ++col) {
            if (row < 0) {
                const qsizetype displayIndex = _historyLayout.size() + row;
                const auto history = _core->scrollbackSnapshot();
                const NovaTerm::DisplayLine* display =
                    displayIndex >= 0 && displayIndex < _historyLayout.size()
                    ? &_historyLayout[displayIndex] : nullptr;
                const NovaTerm::LogicalLine* logical = display
                    ? history.lineById(display->lineId) : nullptr;
                const qsizetype cellIndex = display
                    ? display->startCell + col : -1;
                if (!logical || cellIndex < display->startCell
                    || cellIndex >= display->endCell) {
                    result += QLatin1Char(' ');
                } else if (logical->cells[cellIndex].isWideContinuation()) {
                    continue;
                } else if (logical->cells[cellIndex].chars[0]) {
                    result += cellCharsToString(
                        logical->cells[cellIndex].chars.data(),
                                                NovaTerm::MaxCharsPerCell);
                } else {
                    result += QLatin1Char(' ');
                }
            } else {
                NovaTerm::Cell cell;
                if (!_core->getCell(row, col, cell)) {
                    result += QLatin1Char(' ');
                } else if (cell.isWideContinuation()) {
                    continue;
                } else if (cell.chars[0]) {
                    result += cellCharsToString(cell.chars.data(),
                                                NovaTerm::MaxCharsPerCell);
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
    return isDocumentPositionValid(_selStart) &&
           isDocumentPositionValid(_selEnd) &&
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
    requestOverlayFrame();
}

// ═══════════════════════════════════════════════════════════════════
//  坐标转换
// ═══════════════════════════════════════════════════════════════════

QPoint TerminalRenderer::widgetToCell(const QPoint& pos) const
{
    const int col = std::clamp(qFloor(pos.x() / std::max<qreal>(1.0, _cellWidth)),
                               0, std::max(0, _core->columns() - 1));
    const int widgetRow = qFloor(pos.y() / std::max<qreal>(1.0, _cellHeight));
    const int documentRow = std::clamp(widgetRow - _scrollLine,
                                       -_core->scrollbackLineCount(),
                                       std::max(0, _core->rows() - 1));
    return QPoint(col, documentRow);
}

// ═══════════════════════════════════════════════════════════════════
//  paintEvent
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::initialize(QRhiCommandBuffer* cb)
{
    Q_UNUSED(cb);

    if (_rhi != rhi()) {
        releaseRhiResources();
        _rhi = rhi();
        _capabilities = NovaTerm::RendererCapabilities::detect(_rhi);
        if (!_capabilities.fallbackReason.isEmpty())
            ++_renderStatistics.capabilityFallbacks;
        qInfo() << "TerminalRenderer: GPU glyph renderer initialized with"
                << rhiApiName(api());
    }

    ensureAtlasTexture();
    ensurePipeline();
}

void TerminalRenderer::render(QRhiCommandBuffer* cb)
{
    QElapsedTimer frameTimer;
    frameTimer.start();
    if (!_rhi || !cb || !renderTarget())
        return;

    const QSize pixelSize = colorTexture() ? colorTexture()->pixelSize() : QSize();
    if (pixelSize.isEmpty())
        return;

    ensureAtlasTexture();
    ensurePipeline();
    if (!_atlasTexture || !_pipeline || !_srb)
        return;

    // Take ownership of this frame's requests up front. New requests arriving
    // while commands are built remain queued for the next frame instead of
    // invalidating this iteration or being erased at the end of this one.
    QVector<NovaTerm::DirtyRegion> pendingDirtyRegions;
    bool fullFramePending = false;
    bool overlayPending = false;
    bool explicitFullPending = false;
    int pendingLiveScrollRows = 0;
    quint64 requestedContentRevision = 0;
    {
        const QMutexLocker lock(&_pendingFrameMutex);
        pendingDirtyRegions.swap(_pendingDirtyRegions);
        fullFramePending = std::exchange(_fullFramePending, false);
        // requestFullFrame() and RenderScheduler::frameRequested() are
        // asynchronous. An already queued incremental QRhi frame can render
        // between them; do not let that earlier frame consume the intent that
        // protects a resize/font/resource rebuild from the live-scroll fast
        // path. Retire the intent only together with an actual full frame.
        explicitFullPending = _explicitFullPending;
        if (fullFramePending)
            _explicitFullPending = false;
        overlayPending = std::exchange(_overlayPending, false);
        pendingLiveScrollRows = _scrollDamageHandoff.takePending();
        requestedContentRevision = std::exchange(_pendingContentRevision, 0);
    }

    bool contentPending = fullFramePending
        || !pendingDirtyRegions.isEmpty()
        || pendingLiveScrollRows > 0
        || _commandBuffer.rows() <= 0 || _commandBuffer.columns() <= 0;
    int rows = _commandBuffer.rows();
    int columns = _commandBuffer.columns();
    if (rows <= 0 || columns <= 0) {
        rows = _core->rows();
        columns = _core->columns();
    }
    if (rows <= 0 || columns <= 0)
        return;

    if (_commandBuffer.rows() != rows || _commandBuffer.columns() != columns) {
        _commandBuffer.resize(rows, columns);
        _rowBlockDamageTracker.reset(rows, columns);
        fullFramePending = true;
        overlayPending = true;
    }
    if (!_rowSlotMap.isValidPermutation(rows)) {
        resetWidgetRowMapping(rows);
        fullFramePending = true;
        overlayPending = true;
        contentPending = true;
    }
    if (_rowContentIdentities.size() != rows)
        _rowContentIdentities.fill(0, rows);


    // A scroll callback only says that lines entered scrollback; it does not
    // prove that every retained GPU row can be represented by the same slot
    // permutation. Cursor-positioned Windows shells (PowerShell and Clink in
    // particular) combine scrolls, erases, and rewrites in one publication.
    // Their conservative mode rebuilds the final snapshot atomically. Known
    // line-streaming profiles may opt into the row-slot rotation fast path.
    bool liveScrollRotated = false;
    if (pendingLiveScrollRows > 0) {
        if (_conservativeLiveScrollRendering || explicitFullPending) {
            resetWidgetRowMapping(rows);
            fullFramePending = true;
            overlayPending = true;
            contentPending = true;
            pendingDirtyRegions.clear();
            ++_renderStatistics.revisionPromotedFullFrames;
        } else {
            const int scrollRows = std::min(pendingLiveScrollRows, rows);
            _commandBuffer.rotateRowsUp(scrollRows);
            _rowSlotMap.rotateRowsUp(scrollRows, float(_cellHeight));
            _rowBlockDamageTracker.rotateRowsUp(scrollRows);
            std::rotate(_rowContentIdentities.begin(),
                        _rowContentIdentities.begin() + scrollRows,
                        _rowContentIdentities.end());
            std::fill(_rowContentIdentities.end() - scrollRows,
                      _rowContentIdentities.end(), quint64(0));
            fullFramePending = false;
            pendingDirtyRegions.clear();
            pendingDirtyRegions.push_back(
                {rows - scrollRows, rows, 0, columns});
            _renderStatistics.rowSlotsReused += quint64(rows - scrollRows);
            _renderStatistics.rowSlotsCreated += quint64(scrollRows);
            ++_renderStatistics.mappingOnlyUpdates;
            liveScrollRotated = true;
        }
    }

    QVector<bool> dirtyRows(rows, fullFramePending);
    QVector<QVector<NovaTerm::DirtyColumnSpan>> dirtySpans(rows);
    if (fullFramePending) {
        for (int row = 0; row < rows; ++row)
            dirtySpans[row].push_back({0, columns});
    }
    if (!fullFramePending) {
        for (const NovaTerm::DirtyRegion& region : std::as_const(pendingDirtyRegions)) {
            const int start = std::clamp(region.startRow, 0, rows);
            const int end = std::clamp(region.endRow, 0, rows);
            const int startColumn = std::clamp(
                (region.startColumn / 8) * 8, 0, columns);
            const int endColumn = std::clamp(
                ((region.endColumn + 7) / 8) * 8, 0, columns);
            for (int row = start; row < end; ++row) {
                dirtyRows[row] = true;
                dirtySpans[row].push_back({startColumn, endColumn});
            }
        }
    }

    NovaTerm::RendererSnapshot screen;
    if (contentPending) {
        screen = _core->rendererSnapshot(dirtyRows, _scrollLine,
                                         _scrollAnchorLine,
                                         _scrollAnchorWrap);
        // The parser may publish another batch after its model lock is
        // released but before the queued damage signal reaches the GUI
        // thread. If this snapshot is newer than all damage delivered to the
        // scheduler, sparse rows are not a complete description of it.
        if (!fullFramePending
            && screen.revision > requestedContentRevision) {
            if (screen.visibleRowRevisions.size() == rows
                && screen.visibleRowIdentities.size() == rows) {
                int recoveredRows = 0;
                for (int row = 0; row < rows; ++row) {
                    if (screen.visibleRowRevisions[row]
                            <= requestedContentRevision
                        || screen.visibleRowIdentities[row]
                            == _rowContentIdentities.value(row)) {
                        continue;
                    }
                    if (!dirtyRows[row])
                        ++recoveredRows;
                    dirtyRows[row] = true;
                    dirtySpans[row] = {{0, columns}};
                }
                _renderStatistics.revisionRecoveredRows +=
                    quint64(recoveredRows);
                if (recoveredRows > 0) {
                    screen = _core->rendererSnapshot(
                        dirtyRows, _scrollLine, _scrollAnchorLine,
                        _scrollAnchorWrap);
                }
            } else {
                dirtyRows.fill(true);
                for (int row = 0; row < rows; ++row)
                    dirtySpans[row] = {{0, columns}};
                fullFramePending = true;
                overlayPending = true;
                ++_renderStatistics.revisionPromotedFullFrames;
                screen = _core->rendererSnapshot(
                    dirtyRows, _scrollLine, _scrollAnchorLine,
                    _scrollAnchorWrap);
            }
        }
        if (screen.rows != rows || screen.columns != columns) {
            rows = screen.rows;
            columns = screen.columns;
            _commandBuffer.resize(rows, columns);
            // The model resize is asynchronous. It can become visible only
            // after this frame has already prepared the old row-slot map.
            // Reset both together before rebuilding/uploading; otherwise a
            // shrunken grid may retain slots outside [0, rows), whose zeroed
            // placement entries draw several character rows at y=0.
            resetWidgetRowMapping(rows);
            liveScrollRotated = false;
            dirtyRows.fill(true, rows);
            dirtySpans.resize(rows);
            for (int row = 0; row < rows; ++row)
                dirtySpans[row] = {{0, columns}};
            fullFramePending = true;
            overlayPending = true;
            screen = _core->rendererSnapshot(dirtyRows, _scrollLine,
                                             _scrollAnchorLine,
                                             _scrollAnchorWrap);
        }

        if (liveScrollRotated
            && screen.visibleRowIdentities.size() == rows) {
            const QVector<int> recovered =
                NovaTerm::rowsNeedingRebuildAfterMapping(
                    _rowContentIdentities, screen.visibleRowIdentities,
                    dirtyRows);
            for (const int row : recovered) {
                dirtyRows[row] = true;
                dirtySpans[row] = {{0, columns}};
            }
            if (!recovered.isEmpty()) {
                _renderStatistics.revisionRecoveredRows +=
                    quint64(recovered.size());
                screen = _core->rendererSnapshot(
                    dirtyRows, _scrollLine, _scrollAnchorLine,
                    _scrollAnchorWrap);
            }
        }

        // ConPTY emits cursor-positioned fragments whose damage rectangle can
        // omit columns cleared later in the same parser publication. Compare
        // every dirty row with the renderer's actual 8-column block cache and
        // add only missing changed blocks rather than rebuilding every row.
        if (!liveScrollRotated) {
            for (int row = 0; row < rows; ++row) {
                if (!dirtyRows.value(row))
                    continue;
                const auto cells = screen.visibleRows.value(row);
                dirtySpans[row] = _rowBlockDamageTracker.reconcileRow(
                    row, cells ? cells->constData() : nullptr, columns,
                    std::move(dirtySpans[row]));
            }
        }

    }

    QElapsedTimer commandTimer;
    commandTimer.start();
    quint64 commandsGenerated = 0;
    bool atlasResetDuringBuild = contentPending
        ? rebuildCommandRows(screen, dirtyRows, dirtySpans,
                             commandsGenerated) : false;
    bool staleAtlasRows = contentPending
        && !_commandBuffer.rowsUseAtlasGeneration(_atlasGeneration);
    for (int attempt = 0;
         attempt < 3 && (atlasResetDuringBuild || staleAtlasRows);
         ++attempt) {
        // Never draw cached UVs from a previous atlas generation. Reacquire a
        // complete snapshot and repair every row in the same frame.
        dirtyRows.fill(true);
        for (int row = 0; row < rows; ++row)
            dirtySpans[row] = {{0, columns}};
        fullFramePending = true;
        overlayPending = true;
        screen = _core->rendererSnapshot(dirtyRows, _scrollLine,
                                         _scrollAnchorLine,
                                         _scrollAnchorWrap);
        atlasResetDuringBuild =
            rebuildCommandRows(screen, dirtyRows, dirtySpans,
                               commandsGenerated);
        staleAtlasRows =
            !_commandBuffer.rowsUseAtlasGeneration(_atlasGeneration);
    }
    if (atlasResetDuringBuild || staleAtlasRows) {
        qWarning() << "TerminalRenderer: could not stabilize glyph atlas "
                      "generation for the visible grid";
        requestFullFrame();
        recordCpuFrame(quint64(frameTimer.nsecsElapsed()));
        return;
    }
    if (overlayPending || fullFramePending) {
        const NovaTerm::CursorState cursor = contentPending
            ? screen.cursor : _core->cursorState();
        commandsGenerated += rebuildOverlays(cursor);
    }
    _renderStatistics.commandGenerationNanoseconds +=
        quint64(commandTimer.nsecsElapsed());
    _renderStatistics.commandsGenerated += commandsGenerated;

    const bool bufferReallocated = ensureVertexBuffer(rows, columns);
    if (!_vertexBuffer)
        return;

    QRhiResourceUpdateBatch* resourceUpdates = _rhi->nextResourceUpdateBatch();
    uploadCommands(resourceUpdates, pixelSize, dirtyRows, dirtySpans,
                   bufferReallocated || fullFramePending,
                   overlayPending || fullFramePending || bufferReallocated);
    updatePlacementBuffer(resourceUpdates, pixelSize);
    uploadAtlasChanges(resourceUpdates);

    cb->beginPass(renderTarget(), _scheme.background, {1.0f, 0}, resourceUpdates);
    cb->setGraphicsPipeline(_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));
    cb->setShaderResources(_srb.get());

    if (_backgroundRowStrideVertices > 0) {
        const QRhiCommandBuffer::VertexInput binding(_vertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(4, rows * _backgroundRowStrideVertices);
        ++_renderStatistics.drawCalls;
    }
    const int contentBase = rows * _backgroundRowStrideVertices;
    if (_contentRowStrideVertices > 0) {
        const quint32 offset = quint32(
            contentBase * int(sizeof(GpuInstance)));
        const QRhiCommandBuffer::VertexInput binding(_vertexBuffer.get(), offset);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(4, rows * _contentRowStrideVertices);
        ++_renderStatistics.drawCalls;
    }
    if (_overlayVertexCount > 0) {
        const quint32 offset = quint32(_overlayBaseVertex
                                      * int(sizeof(GpuInstance)));
        const QRhiCommandBuffer::VertexInput binding(_vertexBuffer.get(), offset);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(4, _overlayVertexCount);
        ++_renderStatistics.drawCalls;
    }
    cb->endPass();

    if (contentPending)
        _renderStatistics.lastRenderedRevision = screen.revision;
    ++_frameNumber;
    _renderStatistics.glyphCacheHits = _glyphCache.statistics().hits;
    _renderStatistics.glyphCacheMisses = _glyphCache.statistics().misses;
    _renderStatistics.glyphEvictions =
        _glyphCache.atlas().statistics().pageEvictions;
    _renderStatistics.viewportMappingRevision = _viewportMappingRevision;
    const auto& atlasStatistics = _glyphCache.atlas().statistics();
    const auto& rasterQueueStatistics = _glyphRasterQueue.statistics();
    _renderStatistics.atlasCurrentBytes = atlasStatistics.currentBytes;
    _renderStatistics.atlasPeakBytes = atlasStatistics.peakBytes;
    _renderStatistics.memoryCurrentBytes =
        _renderStatistics.bufferCurrentBytes + atlasStatistics.currentBytes;
    _renderStatistics.memoryPeakBytes = std::max(
        _renderStatistics.memoryPeakBytes,
        _renderStatistics.bufferPeakBytes + atlasStatistics.peakBytes);
    _renderStatistics.glyphRasterQueueDepth =
        quint64(_glyphRasterQueue.size());
    _renderStatistics.glyphRasterQueuePeakDepth =
        quint64(rasterQueueStatistics.peakDepth);
    _renderStatistics.glyphRasterQueueRejected =
        rasterQueueStatistics.rejected;
    _renderStatistics.glyphRasterQueueCancelled =
        rasterQueueStatistics.cancelled;
    _renderStatistics.glyphRasterQueueStaleDropped =
        rasterQueueStatistics.staleDropped;
    recordCpuFrame(quint64(frameTimer.nsecsElapsed()));
}

void TerminalRenderer::recordCpuFrame(quint64 elapsedNanoseconds)
{
    _renderStatistics.cpuFrameNanoseconds += elapsedNanoseconds;
    ++_renderStatistics.framesRendered;
    if (elapsedNanoseconds > kCpuFrameBudgetNanoseconds)
        ++_renderStatistics.cpuFramesOverBudget;

    if (_cpuFrameSamples.size() < kCpuFrameSampleCapacity) {
        _cpuFrameSamples.push_back(elapsedNanoseconds);
        return;
    }
    _cpuFrameSamples[_cpuFrameSampleCursor] = elapsedNanoseconds;
    _cpuFrameSampleCursor =
        (_cpuFrameSampleCursor + 1) % kCpuFrameSampleCapacity;
}

void TerminalRenderer::releaseResources()
{
    releaseRhiResources();
    _rhi = nullptr;
}

// ═══════════════════════════════════════════════════════════════════
//  resizeEvent
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::resizeEvent(QResizeEvent* event)
{
    QRhiWidget::resizeEvent(event);

    recalculateCellSize();
    resizeTerminalToViewport();
    if (_scrollLine > 0)
        _reflowDebounce->start();
    else
        discardHistoryLayout();
    if (_renderScheduler)
        _renderScheduler->setViewport(_core->columns(), _core->rows());
    requestFullFrame();
}

void TerminalRenderer::discardHistoryLayout()
{
    _reflowDebounce->stop();
    _core->cancelScrollbackReflow(_reflowGeneration);
    ++_reflowGeneration;
    _historyLayout.clear();
    _pendingHistoryLayout.clear();
}

void TerminalRenderer::scheduleReflow()
{
    ++_reflowGeneration;
    ++_renderStatistics.scrollbackReflowRequests;
    _pendingHistoryLayout.clear();
    _core->requestScrollbackReflow(_core->columns(), _reflowGeneration, 256);
}

void TerminalRenderer::resizeTerminalToViewport()
{
    const int cols = qFloor(width()  / std::max<qreal>(1.0, _cellWidth));
    const int rows = std::min(
        kMaxGpuPlacementRows,
        qFloor(height() / std::max<qreal>(1.0, _cellHeight)));

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
        scrollToBottom();
        clearSelection();
        _core->resize(cols, rows);
        emit terminalSizeChanged(cols, rows);
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
    event->accept();
}

// ═══════════════════════════════════════════════════════════════════
//  鼠标事件
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::mousePressEvent(QMouseEvent* event)
{
    emit activityDetected();
    setFocus();

    const QPoint cell = widgetToCell(event->pos());
    const int row = cell.y();
    const int col = cell.x();

    if (event->button() == Qt::LeftButton) {
        _selecting = true;
        _selStart  = {row, col};
        _selEnd    = {row, col};
        requestOverlayFrame();
    } else {
        // Non-selection mouse buttons are encoded by TerminalCore.
        _core->processMousePress(event);
    }
}

void TerminalRenderer::mouseMoveEvent(QMouseEvent* event)
{
    if (_selecting) {
        const QPoint cell = widgetToCell(event->pos());
        _selEnd = {cell.y(), cell.x()};
        requestOverlayFrame();
    }
}

void TerminalRenderer::mouseReleaseEvent(QMouseEvent* event)
{
    if (_selecting && event->button() == Qt::LeftButton) {
        _selecting = false;
        const QPoint cell = widgetToCell(event->pos());
        _selEnd = {cell.y(), cell.x()};
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
        const int row = cell.y();
        const int col = cell.x();

        NovaTerm::Cell centerCell;
        if (_core->getCell(row, col, centerCell) && centerCell.chars[0]) {
            // 向左扩展到词边界
            int lc = col;
            while (lc > 0) {
                NovaTerm::Cell c;
                if (!_core->getCell(row, lc - 1, c) || c.chars[0] == 0 || c.chars[0] == ' ')
                    break;
                --lc;
            }
            // 向右扩展到词边界
            int rc = col;
            const int maxCol = _core->columns() - 1;
            while (rc < maxCol) {
                NovaTerm::Cell c;
                if (!_core->getCell(row, rc + 1, c) || c.chars[0] == 0 || c.chars[0] == ' ')
                    break;
                ++rc;
            }
            _selStart = {row, lc};
            _selEnd   = {row, rc};
            _selecting = false;
            copySelection();
            requestOverlayFrame();
        }
    }
}

void TerminalRenderer::wheelEvent(QWheelEvent* event)
{
    const int angleDelta = event->angleDelta().y();
    const int wheelDelta = angleDelta != 0
        ? angleDelta
        : event->pixelDelta().y() * 3;

    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        _wheelAccum = 0;
        _zoomWheelAccum += wheelDelta;
        constexpr int kWheelStep = 120;
        while (_zoomWheelAccum >= kWheelStep) {
            zoomIn();
            _zoomWheelAccum -= kWheelStep;
        }
        while (_zoomWheelAccum <= -kWheelStep) {
            zoomOut();
            _zoomWheelAccum += kWheelStep;
        }
        event->accept();
        return;
    }

    _zoomWheelAccum = 0;
    _wheelAccum += wheelDelta;
    const int lines = _wheelAccum / 120 * kScrollWheelLines;  // 120 = 标准滚轮单位
    if (lines != 0) {
        _wheelAccum -= (lines / kScrollWheelLines) * 120;
        // 正数 lines：向上滚动（回看历史），增加 _scrollLine
        // 负数 lines：向下滚动（返回底部），减少 _scrollLine
        scrollLines(lines);
    }
    event->accept();
}

void TerminalRenderer::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    _core->focusIn();
}

void TerminalRenderer::focusOutEvent(QFocusEvent* event)
{
    QWidget::focusOutEvent(event);
    _core->focusOut();
}

bool TerminalRenderer::focusNextPrevChild(bool next)
{
    Q_UNUSED(next);
    // Tab and Shift+Tab are terminal input (completion / reverse completion),
    // not QWidget focus-navigation keys. Returning false makes QWidget::event()
    // continue dispatching them to keyPressEvent(), so the renderer keeps
    // keyboard focus and libvterm emits the corresponding escape sequence.
    return false;
}

// ═══════════════════════════════════════════════════════════════════
//  内部实现
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::recalculateCellSize()
{
    // For a monospace terminal the cell width follows the font advance, not
    // the visual ink bounds of an individual glyph.
    _cellWidth  = _fm->horizontalAdvance(QLatin1Char('M'));
    if (_cellWidth < 4) _cellWidth = 8;  // 安全下限
    _cellHeight = _fm->height();
    if (_cellHeight < 4) _cellHeight = 16;
}

QPoint TerminalRenderer::cellToWidget(int row, int col) const
{
    return QPoint(qRound(col * _cellWidth), qRound(row * _cellHeight));
}

int TerminalRenderer::cellRowAt(int widgetY) const
{
    return qFloor(widgetY / _cellHeight);
}

int TerminalRenderer::cellColAt(int widgetX) const
{
    return qFloor(widgetX / _cellWidth);
}

bool TerminalRenderer::isDocumentPositionValid(
    const NovaTerm::Position& pos) const
{
    if (pos.row < 0) {
        // scrollback 区域：row 从 -1（最新回滚行）到 -scrollbackLineCount（最旧）
        const qsizetype historyRows = _historyLayout.isEmpty()
            ? _core->scrollbackLineCount() : _historyLayout.size();
        if (pos.row < -historyRows)
            return false;
    } else {
        // 活跃屏幕：row 从 0 到 rows-1
        if (pos.row >= _core->rows())
            return false;
    }
    if (pos.col < 0 || pos.col >= _core->columns())
        return false;
    return true;
}

// ── 渲染 ─────────────────────────────────────────────────────

QShader TerminalRenderer::loadShader(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "TerminalRenderer: failed to open shader" << path << file.errorString();
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

void TerminalRenderer::releaseRhiResources()
{
    _pipeline.reset();
    _srb.reset();
    _vertexBuffer.reset();
    _placementBuffer.reset();
    _vertexBufferSize = 0;
    _bufferBudget.release();
    {
        const QMutexLocker lock(&_pendingFrameMutex);
        _fullFramePending = true;
        _explicitFullPending = true;
        _overlayPending = true;
    }
    _sampler.reset();
    _atlasTexture.reset();
}

void TerminalRenderer::resetGlyphAtlas()
{
    _glyphRasterQueue.cancelBeforeGeneration(_fontManager.generation());
    _glyphCache.clear();
    NovaTerm::GlyphBitmap solid;
    solid.key.faceId = 1;
    solid.key.fontGeneration = _fontManager.generation();
    solid.key.cluster = QStringLiteral("__novaterm_solid__");
    solid.key.pixelSize = 1;
    solid.sourceGeneration = solid.key.fontGeneration;
    solid.image = QImage(1, 1, QImage::Format_RGBA8888);
    solid.image.fill(Qt::white);
    solid.logicalRect = QRectF(0, 0, 1, 1);
    _solidGlyph = _glyphCache.insert(solid, _frameNumber).value_or(
        NovaTerm::GlyphLocation{});
    _atlasDpr = devicePixelRatioF();
    _atlasGeneration = _glyphCache.atlas().generation();
    _atlasTexture.reset();
    _pipeline.reset();
    _srb.reset();
}

void TerminalRenderer::ensureAtlasTexture()
{
    if (!_rhi)
        return;
    if (!qFuzzyCompare(_atlasDpr, devicePixelRatioF())) {
        resetGlyphAtlas();
        // Cached glyph commands refer to atlas pixels generated at the old
        // DPR. Rebuild every row before drawing against the new atlas.
        const QMutexLocker lock(&_pendingFrameMutex);
        _fullFramePending = true;
        _overlayPending = true;
    }
    if (_atlasTexture)
        return;

    const auto& config = _glyphCache.atlas().config();
    const quint64 bytesPerPage = quint64(config.pageSize.width())
        * config.pageSize.height() * 4;
    const int layers = std::max(1, int(config.byteBudget / bytesPerPage));
    _atlasTexture.reset(_rhi->newTextureArray(QRhiTexture::RGBA8, layers,
                                               config.pageSize));
    if (!_atlasTexture->create()) {
        qWarning() << "TerminalRenderer: failed to create glyph atlas texture";
        _atlasTexture.reset();
        return;
    }
    _glyphCache.atlas().markAllDirty();
}

void TerminalRenderer::ensurePipeline()
{
    if (_pipeline || !_rhi || !_atlasTexture)
        return;

    if (!_placementBuffer) {
        const int placementBytes =
            int(sizeof(float) * kPlacementFloatCount);
        _placementBuffer.reset(_rhi->newBuffer(QRhiBuffer::Dynamic,
                                               QRhiBuffer::UniformBuffer,
                                               placementBytes));
        if (!_placementBuffer->create()) {
            _placementBuffer.reset();
            return;
        }
    }

    if (!_sampler) {
        // The glyph atlas is rasterized at the widget's physical DPR. Sampling
        // it without interpolation keeps the cached coverage from being
        // blurred a second time by the GPU.
        _sampler.reset(_rhi->newSampler(QRhiSampler::Nearest,
                                        QRhiSampler::Nearest,
                                        QRhiSampler::None,
                                        QRhiSampler::ClampToEdge,
                                        QRhiSampler::ClampToEdge));
        if (!_sampler->create()) {
            qWarning() << "TerminalRenderer: failed to create QRhi sampler";
            _sampler.reset();
            return;
        }
    }

    _srb.reset(_rhi->newShaderResourceBindings());
    _srb->setBindings({
        QRhiShaderResourceBinding::sampledTexture(0,
                                                  QRhiShaderResourceBinding::FragmentStage,
                                                  _atlasTexture.get(),
                                                  _sampler.get()),
        QRhiShaderResourceBinding::uniformBuffer(
            1, QRhiShaderResourceBinding::VertexStage,
            _placementBuffer.get())
    });
    if (!_srb->create()) {
        qWarning() << "TerminalRenderer: failed to create QRhi shader bindings";
        _srb.reset();
        return;
    }

    _pipeline.reset(_rhi->newGraphicsPipeline());
    _pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/src/renderer/shaders/terminal_texture.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/src/renderer/shaders/terminal_texture.frag.qsb"))}
    });
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({{int(sizeof(GpuInstance)),
                              QRhiVertexInputBinding::PerInstance}});
    inputLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float4, 0},
        {0, 1, QRhiVertexInputAttribute::Float4, 4 * int(sizeof(float))},
        {0, 2, QRhiVertexInputAttribute::Float4, 8 * int(sizeof(float))},
        {0, 3, QRhiVertexInputAttribute::Float4, 12 * int(sizeof(float))}
    });
    _pipeline->setVertexInputLayout(inputLayout);
    _pipeline->setShaderResourceBindings(_srb.get());
    _pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    _pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    _pipeline->setTargetBlends({blend});

    if (!_pipeline->create()) {
        qWarning() << "TerminalRenderer: failed to create QRhi graphics pipeline";
        _pipeline.reset();
        return;
    }
}

NovaTerm::GlyphLocation TerminalRenderer::ensureGlyph(
    const QString& text, bool bold, int cellSpan)
{
    bool emoji = false;
    for (char32_t scalar : text.toUcs4()) {
        if (scalar >= 0x1f000) {
            emoji = true;
            break;
        }
    }
    const NovaTerm::GlyphKey key = _fontManager.makeKey(
        text, bold, false, cellSpan, devicePixelRatioF(),
        emoji ? NovaTerm::GlyphRenderMode::Color
              : NovaTerm::GlyphRenderMode::Grayscale);
    if (auto found = _glyphCache.find(key, _frameNumber))
        return *found;
    const auto selection = _fontManager.select(text, bold, false);
    _glyphRasterQueue.enqueue({key, selection.font, _cellWidth,
                               _cellHeight, true});
    const auto task = _glyphRasterQueue.take();
    NovaTerm::GlyphBitmap bitmap = task
        ? _glyphRasterizer.rasterize(task->key, task->font,
                                     task->cellWidth, task->cellHeight)
        : _glyphRasterizer.rasterize(key, selection.font,
                                     _cellWidth, _cellHeight);
    ++_renderStatistics.glyphRasters;
    const auto inserted = _glyphCache.insert(bitmap, _frameNumber);
    _atlasGeneration = _glyphCache.atlas().generation();
    return inserted.value_or(_solidGlyph);
}

void TerminalRenderer::appendQuad(const QRectF& rect, const QRectF& uvRect,
                                  const QColor& color, const QSize& pixelSize)
{
    Q_UNUSED(pixelSize);
    const float red = color.redF();
    const float green = color.greenF();
    const float blue = color.blueF();
    const float alpha = color.alphaF();
    _instances.push_back({float(rect.left()), float(rect.top()),
                          float(rect.right()), float(rect.bottom()),
                          float(uvRect.left()), float(uvRect.top()),
                          float(uvRect.right()), float(uvRect.bottom()),
                          red, green, blue, alpha, 0.0f, -1.0f, 0.0f, 0.0f});
}

void TerminalRenderer::appendSolidRect(const QRectF& rect, const QColor& color,
                                       const QSize& pixelSize)
{
    const qreal atlasWidth = _glyphCache.atlas().config().pageSize.width();
    const qreal atlasHeight = _glyphCache.atlas().config().pageSize.height();
    appendQuad(rect,
               QRectF(_solidGlyph.pixelRect.left() / atlasWidth,
                      _solidGlyph.pixelRect.top() / atlasHeight,
                      _solidGlyph.pixelRect.width() / atlasWidth,
                      _solidGlyph.pixelRect.height() / atlasHeight),
               color, pixelSize);
}

void TerminalRenderer::appendTexturedRect(const QRectF& rect, const QRect& atlasRect,
                                          const QColor& color, const QSize& pixelSize)
{
    const qreal atlasWidth = _glyphCache.atlas().config().pageSize.width();
    const qreal atlasHeight = _glyphCache.atlas().config().pageSize.height();
    appendQuad(rect,
               QRectF(atlasRect.left() / atlasWidth,
                      atlasRect.top() / atlasHeight,
                      atlasRect.width() / atlasWidth,
                      atlasRect.height() / atlasHeight),
               color, pixelSize);
}

NovaTerm::RenderCommand TerminalRenderer::makeSolidCommand(
    NovaTerm::RenderCommandType type,
    const QRectF& rect,
    const QColor& color) const
{
    const qreal atlasWidth = _glyphCache.atlas().config().pageSize.width();
    const qreal atlasHeight = _glyphCache.atlas().config().pageSize.height();
    return {
        type,
        rect,
        QRectF(_solidGlyph.pixelRect.left() / atlasWidth,
               _solidGlyph.pixelRect.top() / atlasHeight,
               _solidGlyph.pixelRect.width() / atlasWidth,
               _solidGlyph.pixelRect.height() / atlasHeight),
        color,
        _solidGlyph.pageId,
        _solidGlyph.pageGeneration
    };
}

void TerminalRenderer::requestFullFrame()
{
    {
        const QMutexLocker lock(&_pendingFrameMutex);
        _explicitFullPending = true;
    }
    if (_renderScheduler)
        _renderScheduler->scheduleFullFrame(_core->modelRevision());
    else {
        {
            const QMutexLocker lock(&_pendingFrameMutex);
            _fullFramePending = true;
            _overlayPending = true;
            _pendingContentRevision = std::max(_pendingContentRevision,
                                               _core->modelRevision());
        }
        update();
    }
}

void TerminalRenderer::requestOverlayFrame()
{
    if (_renderScheduler)
        _renderScheduler->scheduleOverlay();
    else {
        {
            const QMutexLocker lock(&_pendingFrameMutex);
            _overlayPending = true;
        }
        update();
    }
}

void TerminalRenderer::appendCellCommands(
    qreal x,
    qreal y,
    const NovaTerm::Cell& cell,
    const QColor* defaultForegroundOverride,
    QVector<NovaTerm::RenderCommand>& backgrounds,
    QVector<NovaTerm::RenderCommand>& contents)
{
    const int cellColumn = qRound(x / std::max<qreal>(1.0, _cellWidth));
    const int cellSpan = std::max(1, static_cast<int>(cell.width));
    const qreal paintWidth = _cellWidth * cellSpan;
    QColor foreground = terminalColorToQColor(cell.foreground, true);
    if (defaultForegroundOverride
        && cell.foreground.type == NovaTerm::ColorType::Default
        && !cell.attributes.reverse) {
        foreground = *defaultForegroundOverride;
    }
    QColor background = terminalColorToQColor(cell.background, false);
    if (cell.attributes.reverse)
        std::swap(foreground, background);
    backgrounds.push_back(makeSolidCommand(
        NovaTerm::RenderCommandType::BackgroundRect,
        QRectF(x, y, paintWidth, _cellHeight), background));
    backgrounds.last().cellColumn = cellColumn;

    if (cell.attributes.conceal)
        return;
    if (cell.chars[0] != 0 && cell.chars[0] != ' ') {
        const QString text = cellCharsToString(cell.chars.data(),
                                               NovaTerm::MaxCharsPerCell);
        if (!text.isEmpty()) {
            const auto glyph =
                ensureGlyph(text, cell.attributes.bold, cellSpan);
            const qreal atlasWidth =
                _glyphCache.atlas().config().pageSize.width();
            const qreal atlasHeight =
                _glyphCache.atlas().config().pageSize.height();
            contents.push_back({
                NovaTerm::RenderCommandType::GlyphInstance,
                glyph.logicalRect.translated(x, y),
                QRectF(glyph.pixelRect.left() / atlasWidth,
                       glyph.pixelRect.top() / atlasHeight,
                       glyph.pixelRect.width() / atlasWidth,
                       glyph.pixelRect.height() / atlasHeight),
                foreground,
                glyph.pageId,
                glyph.pageGeneration,
                cellColumn,
                glyph.format == NovaTerm::GlyphPixelFormat::Rgba8
            });
        }
    }
    if (cell.attributes.underline) {
        const qreal underlineY = y + _fm->ascent() + 2;
        contents.push_back(makeSolidCommand(
            NovaTerm::RenderCommandType::Underline,
            QRectF(x, underlineY, paintWidth, 1), foreground));
        contents.last().cellColumn = cellColumn;
        if (cell.attributes.underlineStyle == NovaTerm::UnderlineStyle::Double)
            contents.push_back(makeSolidCommand(
                NovaTerm::RenderCommandType::Underline,
                QRectF(x, underlineY + 2, paintWidth, 1), foreground));
        if (!contents.isEmpty())
            contents.last().cellColumn = cellColumn;
    }
    if (cell.attributes.strike)
        contents.push_back(makeSolidCommand(
            NovaTerm::RenderCommandType::Strike,
            QRectF(x, y + _cellHeight / 2, paintWidth, 1), foreground));
    if (!contents.isEmpty() && contents.last().cellColumn < 0)
        contents.last().cellColumn = cellColumn;
}

bool TerminalRenderer::rebuildCommandRows(
    const NovaTerm::RendererSnapshot& screen,
    const QVector<bool>& dirtyRows,
    const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
    quint64& commandsGenerated)
{
    const quint64 generationBefore = _atlasGeneration;
    for (int row = 0; row < dirtyRows.size(); ++row) {
        if (!dirtyRows[row])
            continue;
        rebuildCommandRow(row, screen, dirtySpans.value(row));
        const auto& commands = _commandBuffer.row(row);
        commandsGenerated += quint64(commands.backgrounds.size()
                                     + commands.contents.size());
        ++_renderStatistics.rowsRebuilt;
        _renderStatistics.dirtyBlocksRebuilt += quint64(
            std::max<qsizetype>(1, dirtySpans.value(row).size()));
    }
    return _atlasGeneration != generationBefore;
}

void TerminalRenderer::rebuildCommandRow(
    int widgetRow,
    const NovaTerm::RendererSnapshot& screen,
    const QVector<NovaTerm::DirtyColumnSpan>& dirtySpans)
{
    QVector<NovaTerm::RenderCommand> backgrounds;
    QVector<NovaTerm::RenderCommand> contents;
    backgrounds.reserve(screen.columns);
    contents.reserve(screen.columns);

    // A token entering or leaving a row can change the semantic colour of all
    // default-colour cells on that row, so incremental column reuse is unsafe
    // while highlighting is enabled.
    const bool replaceAll = !_highlightRules.isEmpty() || dirtySpans.isEmpty()
        || (dirtySpans.size() == 1 && dirtySpans.front().startColumn <= 0
            && dirtySpans.front().endColumn >= screen.columns);
    auto isDirty = [&dirtySpans, replaceAll](int column) {
        if (replaceAll)
            return true;
        for (const auto& span : dirtySpans) {
            if (column >= span.startColumn && column < span.endColumn)
                return true;
        }
        return false;
    };
    if (!replaceAll && widgetRow < _commandBuffer.rows()) {
        const auto& old = _commandBuffer.row(widgetRow);
        for (const auto& command : old.backgrounds) {
            if (!isDirty(command.cellColumn))
                backgrounds.push_back(command);
        }
        for (const auto& command : old.contents) {
            if (!isDirty(command.cellColumn))
                contents.push_back(command);
        }
    }

    const std::optional<QColor> rowColor =
        rowHighlightColor(widgetRow, screen);
    for (int column = 0; column < screen.columns; ++column) {
        if (!isDirty(column))
            continue;
        const qreal x = column * _cellWidth;
        const qreal y = 0.0;
        const NovaTerm::Cell* cell = screen.cellAt(widgetRow, column);
        if (cell && !cell->isWideContinuation())
            appendCellCommands(x, y, *cell,
                               rowColor ? &*rowColor : nullptr,
                               backgrounds, contents);
    }
    const auto byColumn = [](const NovaTerm::RenderCommand& a,
                             const NovaTerm::RenderCommand& b) {
        return a.cellColumn < b.cellColumn;
    };
    std::stable_sort(backgrounds.begin(), backgrounds.end(), byColumn);
    std::stable_sort(contents.begin(), contents.end(), byColumn);
    _commandBuffer.replaceRow(widgetRow, std::move(backgrounds),
                              std::move(contents), _atlasGeneration);
    if (widgetRow >= 0 && widgetRow < _rowContentIdentities.size())
        _rowContentIdentities[widgetRow] =
            screen.visibleRowIdentities.value(widgetRow);
}

quint64 TerminalRenderer::rebuildOverlays(
    const NovaTerm::CursorState& cursor)
{
    QVector<NovaTerm::RenderCommand> overlays;
    overlays.reserve(_core->rows() + 1);
    appendSelectionCommands(overlays);
    appendSearchCommands(overlays);
    appendCursorCommand(overlays, cursor);
    const quint64 commandCount = quint64(overlays.size());
    _commandBuffer.replaceOverlays(std::move(overlays));
    return commandCount;
}

void TerminalRenderer::setSearchMatches(
    QVector<NovaTerm::SearchMatch> matches, quint64 generation)
{
    if (generation < _searchGeneration)
        return;
    _searchMatchesByLine.clear();
    _searchGeneration = generation;
    for (NovaTerm::SearchMatch& match : matches)
        _searchMatchesByLine[match.lineId].push_back(std::move(match));
    requestOverlayFrame();
}

void TerminalRenderer::appendSearchMatches(
    QVector<NovaTerm::SearchMatch> matches, quint64 generation)
{
    if (generation < _searchGeneration)
        return;
    if (generation > _searchGeneration) {
        _searchMatchesByLine.clear();
        _searchGeneration = generation;
    }
    for (NovaTerm::SearchMatch& match : matches)
        _searchMatchesByLine[match.lineId].push_back(std::move(match));
    requestOverlayFrame();
}

void TerminalRenderer::clearSearchMatches()
{
    _searchMatchesByLine.clear();
    requestOverlayFrame();
}

qsizetype TerminalRenderer::searchMatchCount() const
{
    qsizetype count = 0;
    for (auto it = _searchMatchesByLine.cbegin();
         it != _searchMatchesByLine.cend(); ++it) {
        count += it.value().size();
    }
    return count;
}

void TerminalRenderer::appendSearchCommands(
    QVector<NovaTerm::RenderCommand>& commands)
{
    if (_searchMatchesByLine.isEmpty() || _scrollLine <= 0)
        return;
    const QColor color(255, 196, 0, 105);
    if (!_historyLayout.isEmpty() && _scrollLine > 0) {
        const qsizetype first = std::max<qsizetype>(
            0, _historyLayout.size() - _scrollLine);
        const qsizetype last = std::min<qsizetype>(
            _historyLayout.size(), first + _core->rows());
        for (qsizetype row = first; row < last; ++row) {
            const auto& display = _historyLayout[row];
            const auto found = _searchMatchesByLine.constFind(display.lineId);
            if (found == _searchMatchesByLine.cend())
                continue;
            for (const NovaTerm::SearchMatch& match : found.value()) {
                if (match.endCell <= display.startCell
                    || match.startCell >= display.endCell) {
                    continue;
                }
                const qsizetype start = std::max(
                    match.startCell, display.startCell) - display.startCell;
                const qsizetype end = std::min(
                    match.endCell, display.endCell) - display.startCell;
                const int widgetRow = int(row - first);
                commands.push_back(makeSolidCommand(
                    NovaTerm::RenderCommandType::SearchOverlay,
                    QRectF(start * _cellWidth, widgetRow * _cellHeight,
                           (end - start) * _cellWidth, _cellHeight), color));
            }
        }
        return;
    }
    const auto history = _core->scrollbackSnapshot();
    for (auto it = _searchMatchesByLine.cbegin();
         it != _searchMatchesByLine.cend(); ++it) {
        const qsizetype documentRow = history.rowForLineId(it.key());
        if (documentRow < 0)
            continue;
        const int widgetRow = int(documentRow - history.lineCount())
            + _scrollLine;
        if (widgetRow < 0 || widgetRow >= _core->rows())
            continue;
        for (const NovaTerm::SearchMatch& match : it.value()) {
            const qsizetype start = std::clamp<qsizetype>(
                match.startCell, 0, _core->columns());
            const qsizetype end = std::clamp<qsizetype>(
                match.endCell, start, _core->columns());
            commands.push_back(makeSolidCommand(
                NovaTerm::RenderCommandType::SearchOverlay,
                QRectF(start * _cellWidth, widgetRow * _cellHeight,
                       (end - start) * _cellWidth, _cellHeight), color));
        }
    }
}

void TerminalRenderer::appendCursorCommand(
    QVector<NovaTerm::RenderCommand>& commands,
    const NovaTerm::CursorState& cursor)
{
    if (!cursor.visible || _scrollLine != 0
        || (cursor.blink && !_cursorBlinkVisible))
        return;
    const auto position = cursor.position;
    if (position.row < 0 || position.row >= _core->rows()
        || position.col < 0 || position.col >= _core->columns())
        return;

    const QColor color = _scheme.cursorColor.isValid()
        ? _scheme.cursorColor : _scheme.foreground;
    const QPointF point(position.col * _cellWidth, position.row * _cellHeight);
    QRectF cursorRect(point.x(), point.y(), _cellWidth, _cellHeight);
    if (cursor.shape == NovaTerm::CursorShape::Underline)
        cursorRect = QRectF(point.x(), point.y() + _cellHeight - 2, _cellWidth, 2);
    else if (cursor.shape == NovaTerm::CursorShape::BarLeft)
        cursorRect = QRectF(point.x(), point.y(), 2, _cellHeight);
    commands.push_back(makeSolidCommand(
        NovaTerm::RenderCommandType::Cursor, cursorRect, color));
}

void TerminalRenderer::appendSelectionCommands(
    QVector<NovaTerm::RenderCommand>& commands)
{
    if (!hasSelection())
        return;
    NovaTerm::Position start = _selStart;
    NovaTerm::Position end = _selEnd;
    if (end < start)
        std::swap(start, end);
    const QColor selectionColor = _scheme.selectionColor.isValid()
        ? _scheme.selectionColor : QColor(84, 107, 138, 128);
    for (int row = start.row; row <= end.row; ++row) {
        const int widgetRow = row + _scrollLine;
        if (widgetRow < 0 || widgetRow >= _core->rows())
            continue;
        const int firstColumn = row == start.row ? start.col : 0;
        const int lastColumn = row == end.row ? end.col : _core->columns() - 1;
        const QPointF topLeft(firstColumn * _cellWidth, widgetRow * _cellHeight);
        const QPointF bottomRight((lastColumn + 1) * _cellWidth,
                                  widgetRow * _cellHeight);
        commands.push_back(makeSolidCommand(
            NovaTerm::RenderCommandType::SelectionOverlay,
            QRectF(topLeft.x(), topLeft.y(),
                   bottomRight.x() - topLeft.x(), _cellHeight),
            selectionColor));
    }
}

void TerminalRenderer::appendCommandVertices(
    const NovaTerm::RenderCommand& command,
    const QSize& pixelSize)
{
    appendQuad(command.rect, command.uvRect, command.color, pixelSize);
}

bool TerminalRenderer::ensureVertexBuffer(int rows, int columns)
{
    int backgroundStride = columns;
    int contentStride = columns * 4;
    for (int row = 0; row < _commandBuffer.rows(); ++row) {
        backgroundStride = std::max(
            backgroundStride,
            int(_commandBuffer.row(row).backgrounds.size()));
        contentStride = std::max(
            contentStride,
            int(_commandBuffer.row(row).contents.size()));
    }
    // Capacities only grow during a resource lifetime. A temporary complex
    // row must not move every following row back and forth between offsets.
    backgroundStride = std::max(backgroundStride,
                                _backgroundRowStrideVertices);
    contentStride = std::max(contentStride, _contentRowStrideVertices);
    const int overlayBase = rows * (backgroundStride + contentStride);
    const int overlayCapacity = std::max(
        std::max(rows + 2,
                 int(_commandBuffer.overlays().size())),
        _overlayCapacityVertices);
    const int requiredBytes =
        (overlayBase + overlayCapacity) * int(sizeof(GpuInstance));
    const bool layoutChanged =
        _backgroundRowStrideVertices != backgroundStride
        || _contentRowStrideVertices != contentStride
        || _overlayBaseVertex != overlayBase
        || _overlayCapacityVertices != overlayCapacity;

    _backgroundRowStrideVertices = backgroundStride;
    _contentRowStrideVertices = contentStride;
    _overlayBaseVertex = overlayBase;
    _overlayCapacityVertices = overlayCapacity;

    if (_vertexBuffer && _vertexBufferSize >= requiredBytes)
        return layoutChanged;

    const auto capacity = _bufferBudget.capacityFor(quint64(requiredBytes));
    if (!capacity) {
        qWarning() << "TerminalRenderer: instance buffer budget exceeded"
                   << requiredBytes << "bytes required";
        return false;
    }
    _vertexBuffer.reset();
    _vertexBufferSize = int(*capacity);
    _vertexBuffer.reset(_rhi->newBuffer(QRhiBuffer::Dynamic,
                                        QRhiBuffer::VertexBuffer,
                                        _vertexBufferSize));
    if (!_vertexBuffer->create()) {
        qWarning() << "TerminalRenderer: failed to create QRhi vertex buffer";
        _vertexBuffer.reset();
        _vertexBufferSize = 0;
        return true;
    }
    ++_renderStatistics.vertexBufferReallocations;
    _renderStatistics.bufferCurrentBytes = quint64(_vertexBufferSize);
    _renderStatistics.bufferPeakBytes = std::max(
        _renderStatistics.bufferPeakBytes, quint64(_vertexBufferSize));
    return true;
}

QColor TerminalRenderer::highlightColor(
    NovaTerm::TerminalHighlightRole role) const
{
    switch (role) {
    case NovaTerm::TerminalHighlightRole::Error:
        return _scheme.palette[9];
    case NovaTerm::TerminalHighlightRole::Warning:
        return _scheme.palette[11];
    case NovaTerm::TerminalHighlightRole::Success:
        return _scheme.palette[10];
    case NovaTerm::TerminalHighlightRole::Prompt:
        return _scheme.palette[14];
    }
    return _scheme.foreground;
}

std::optional<QColor> TerminalRenderer::rowHighlightColor(
    int widgetRow,
    const NovaTerm::RendererSnapshot& screen) const
{
    if (_highlightRules.isEmpty())
        return std::nullopt;

    QString text;
    text.reserve(screen.columns);
    for (int column = 0; column < screen.columns; ++column) {
        const NovaTerm::Cell* cell = screen.cellAt(widgetRow, column);
        if (!cell || cell->isWideContinuation())
            continue;
        const QString cellText = cellCharsToString(
            cell->chars.data(), NovaTerm::MaxCharsPerCell);
        if (cellText.isEmpty())
            text.append(QLatin1Char(' '));
        else
            text.append(cellText);
    }

    const auto role = NovaTerm::matchTerminalHighlight(_highlightRules, text);
    return role ? std::optional<QColor>(highlightColor(*role)) : std::nullopt;
}

void TerminalRenderer::resetWidgetRowMapping(int rows)
{
    rows = std::max(0, rows);
    _rowSlotMap.resetSequential(rows, float(_cellHeight));
    _rowContentIdentities.fill(0, rows);
    _rowBlockDamageTracker.reset(rows, _commandBuffer.columns());
    ++_viewportMappingRevision;
}

void TerminalRenderer::updatePlacementBuffer(
    QRhiResourceUpdateBatch* updates, const QSize& pixelSize)
{
    if (!_placementBuffer || !updates)
        return;
    QVector<float> values(kPlacementFloatCount, 0.0f);
    values[0] = float(pixelSize.width());
    values[1] = float(pixelSize.height());
    values[2] = float(devicePixelRatioF());
    values[3] = float(std::min(kMaxGpuPlacementRows,
                               _commandBuffer.rows()));
    // 后端 NDC Y/depth 修正矩阵（QRhi clipSpaceCorrMatrix）。OpenGL/D3D 为
    // 恒等，Vulkan/Metal 翻转 Y；缺失时整帧垂直镜像，滚动方向完全反向。
    const QMatrix4x4 clipSpaceCorr = _rhi->clipSpaceCorrMatrix();
    const float* matrix = clipSpaceCorr.constData();
    for (int index = 0; index < 16; ++index)
        values[kPlacementMatrixOffset + index] = matrix[index];
    for (const NovaTerm::RowPlacement& placement
         : _rowSlotMap.placements()) {
        if (placement.gpuSlot >= 0
            && placement.gpuSlot < kMaxGpuPlacementRows) {
            values[kPlacementRowOffset + placement.gpuSlot * 4]
                = placement.yTransform;
        }
    }
    updates->updateDynamicBuffer(_placementBuffer.get(), 0,
                                 int(values.size() * sizeof(float)),
                                 values.constData());
    _renderStatistics.gpuUploadBytes +=
        quint64(values.size() * sizeof(float));
}

void TerminalRenderer::uploadAtlasChanges(QRhiResourceUpdateBatch* updates)
{
    if (!_atlasTexture || !updates)
        return;
    NovaTerm::GlyphAtlas& atlas = _glyphCache.atlas();
    // Resource recovery/full invalidation is a separately measured cold path.
    // Complete it atomically so no valid command samples a not-yet-resident
    // page; ordinary incremental rects retain the configured frame budget.
    const quint64 uploadBudget = atlas.hasFullPageUploads()
        ? std::numeric_limits<quint64>::max() : 0;
    const auto uploads = atlas.takeUploads(uploadBudget);
    for (const NovaTerm::GlyphAtlasUpload& upload : uploads) {
        QRhiTextureSubresourceUploadDescription subresource(upload.image);
        subresource.setDestinationTopLeft(upload.rect.topLeft());
        QRhiTextureUploadDescription description(
            QRhiTextureUploadEntry(upload.pageId, 0, subresource));
        updates->uploadTexture(_atlasTexture.get(), description);
        const quint64 bytes = upload.bytes();
        _renderStatistics.atlasUploadBytes += bytes;
        _renderStatistics.gpuUploadBytes += bytes;
    }
    // Upload budgets may defer a page/rect. Ensure eventual residency even
    // when no new terminal Damage or Overlay event arrives.
    if (atlas.hasPendingUploads()) {
        // QRhiWidget may coalesce update() called from inside its active
        // render callback. Queue it onto the GUI event loop so it represents
        // a distinct follow-up frame.
        QMetaObject::invokeMethod(this, [this]() { requestOverlayFrame(); },
                                  Qt::QueuedConnection);
    }
}

void TerminalRenderer::uploadCommands(
    QRhiResourceUpdateBatch* updates,
    const QSize& pixelSize,
    const QVector<bool>& dirtyRows,
    const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
    bool uploadAllRows,
    bool overlayDirty)
{
    const int rows = _commandBuffer.rows();
    const int contentBase = rows * _backgroundRowStrideVertices;
    for (int row = 0; row < rows; ++row) {
        if (!uploadAllRows && !dirtyRows.value(row))
            continue;

        const NovaTerm::RenderCommandRow& commands = _commandBuffer.row(row);
        const int slot = _rowSlotMap.slotForWidgetRow(row);
        if (slot < 0)
            continue;
        QVector<NovaTerm::DirtyColumnSpan> spans = uploadAllRows
            ? QVector<NovaTerm::DirtyColumnSpan>{{0, _commandBuffer.columns()}}
            : dirtySpans.value(row);
        if (spans.isEmpty())
            spans.push_back({0, _commandBuffer.columns()});
        for (const auto& rawSpan : std::as_const(spans)) {
            const int start = std::clamp(rawSpan.startColumn, 0,
                                         _commandBuffer.columns());
            const int end = std::clamp(rawSpan.endColumn, start,
                                       _commandBuffer.columns());
            if (start >= end)
                continue;
            const int cellCount = end - start;
            const int backgroundVertexCount =
                NovaTerm::rowUploadVertexCount(
                    cellCount, _backgroundRowStrideVertices, uploadAllRows);
            _instances.fill(GpuInstance{}, backgroundVertexCount);
            for (const NovaTerm::RenderCommand& command : commands.backgrounds) {
                if (command.cellColumn < start || command.cellColumn >= end)
                    continue;
                const int index = command.cellColumn - start;
                const int oldSize = _instances.size();
                _instances.resize(index);
                appendCommandVertices(command, pixelSize);
                GpuInstance value = _instances.takeLast();
                _instances.resize(oldSize);
                value.atlasPage = float(command.atlasPage);
                value.rowSlot = float(slot);
                value.flags = command.colorGlyph ? 1.0f : 0.0f;
                _instances[index] = value;
            }
            const int backgroundBytes = int(_instances.size()
                                            * sizeof(GpuInstance));
            const int backgroundOffset =
                (slot * _backgroundRowStrideVertices + start)
                * int(sizeof(GpuInstance));
            updates->updateDynamicBuffer(_vertexBuffer.get(), backgroundOffset,
                                         backgroundBytes,
                                         _instances.constData());
            _renderStatistics.gpuUploadBytes += quint64(backgroundBytes);
            _renderStatistics.contentUploadBytes += quint64(backgroundBytes);

            const int contentVertexCount = NovaTerm::rowUploadVertexCount(
                cellCount * 4, _contentRowStrideVertices, uploadAllRows);
            _instances.fill(GpuInstance{}, contentVertexCount);
            QVector<int> perCell(cellCount, 0);
            for (const NovaTerm::RenderCommand& command : commands.contents) {
                if (command.cellColumn < start || command.cellColumn >= end)
                    continue;
                const int cell = command.cellColumn - start;
                if (perCell[cell] >= 4)
                    continue;
                const int index = cell * 4 + perCell[cell]++;
                const int oldSize = _instances.size();
                _instances.resize(index);
                appendCommandVertices(command, pixelSize);
                GpuInstance value = _instances.takeLast();
                _instances.resize(oldSize);
                value.atlasPage = float(command.atlasPage);
                value.rowSlot = float(slot);
                value.flags = command.colorGlyph ? 1.0f : 0.0f;
                _instances[index] = value;
            }
            const int contentBytes = int(_instances.size()
                                         * sizeof(GpuInstance));
            const int contentOffset =
                (contentBase + slot * _contentRowStrideVertices + start * 4)
                * int(sizeof(GpuInstance));
            updates->updateDynamicBuffer(_vertexBuffer.get(), contentOffset,
                                         contentBytes,
                                         _instances.constData());
            _renderStatistics.gpuUploadBytes += quint64(contentBytes);
            _renderStatistics.contentUploadBytes += quint64(contentBytes);
        }
    }

    if (!overlayDirty)
        return;
    _instances.clear();
    _instances.reserve(_commandBuffer.overlays().size());
    for (const NovaTerm::RenderCommand& command : _commandBuffer.overlays()) {
        appendCommandVertices(command, pixelSize);
        _instances.last().atlasPage = float(command.atlasPage);
        _instances.last().rowSlot = -1.0f;
        _instances.last().flags = command.colorGlyph ? 1.0f : 0.0f;
    }
    _overlayVertexCount = _instances.size();
    if (!_instances.isEmpty()) {
        const int bytes = int(_instances.size() * sizeof(GpuInstance));
        const int offset = _overlayBaseVertex * int(sizeof(GpuInstance));
        updates->updateDynamicBuffer(_vertexBuffer.get(), offset, bytes,
                                     _instances.constData());
        _renderStatistics.gpuUploadBytes += quint64(bytes);
    }
}

#if 0
void TerminalRenderer::renderTerminalFrame(QImage& frame)
{
    if (frame.isNull())
        return;

    frame.fill(_scheme.background);

    QPainter p(&frame);
    p.scale(devicePixelRatioF(), devicePixelRatioF());
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::Antialiasing, false);

    renderCells(p, rect());
    renderSelection(p);
    renderCursor(p);
}

void TerminalRenderer::renderCells(QPainter& p, const QRect& dirty)
{
    const int visRows    = _core->rows();
    const int cols       = _core->columns();
    const int sbCount    = _core->scrollbackLineCount();
    const int totalLines = sbCount + visRows;

    const int startWidgetRow = std::max(0, dirty.top() / _cellHeight);
    const int endWidgetRow   = std::min(visRows, (dirty.bottom() / _cellHeight) + 1);

    for (int widgetRow = startWidgetRow; widgetRow < endWidgetRow; ++widgetRow) {
        const int screenRow = widgetRow - _scrollLine;

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
                    if (sc.isWideContinuation())
                        continue;
                    renderCell(p, x, y, sc.chars.data(), sc.width,
                               sc.attributes, sc.foreground, sc.background);
                } else {
                    p.fillRect(x, y, _cellWidth, _cellHeight, _scheme.background);
                }
            } else {
                // ── 活跃屏幕区域 ──────────────────────────────
                NovaTerm::Cell cell;
                if (_core->getCell(screenRow, col, cell)) {
                    if (cell.isWideContinuation())
                        continue;
                    renderCell(p, x, y, cell.chars.data(), cell.width,
                               cell.attributes, cell.foreground, cell.background);
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
                                   const NovaTerm::CellAttributes& attrs,
                                   const NovaTerm::TerminalColor& fg_vc,
                                   const NovaTerm::TerminalColor& bg_vc)
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

    const QString text =
        cellCharsToString(chars, NovaTerm::MaxCharsPerCell);
    if (text.isEmpty()) return;

    // 文字基线对齐
    const qreal textY = y + _fm->ascent();
    p.drawText(QPointF(x, textY), text);

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
    NovaTerm::Cell cell;
    QColor cellFg = _scheme.foreground;
    if (_core->getCell(cpos.row, cpos.col, cell) && cell.chars[0]) {
        cellFg = terminalColorToQColor(cell.foreground, true);
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

    NovaTerm::Position start = _selStart;
    NovaTerm::Position end = _selEnd;
    if (end < start)
        std::swap(start, end);

    const QColor selColor = _scheme.selectionColor.isValid()
        ? _scheme.selectionColor
        : QColor(84, 107, 138, 128);

    for (int row = start.row; row <= end.row; ++row) {
        const int widgetRow = row + _scrollLine;
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

#endif

QColor TerminalRenderer::terminalColorToQColor(
    const NovaTerm::TerminalColor& color, bool foreground) const
{
    if (color.type == NovaTerm::ColorType::Rgb) {
        return QColor(color.red, color.green, color.blue);
    }
    if (color.type == NovaTerm::ColorType::Indexed) {
        const int idx = color.index;
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
    return foreground ? _scheme.foreground : _scheme.background;
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
