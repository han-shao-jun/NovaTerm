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
#include <QMutexLocker>
#include <rhi/qshader.h>
#include <rhi/qrhi.h>
#include <algorithm>
#include <utility>

// 滚动步长（行数）
static constexpr int kScrollWheelLines = 3;
static constexpr int kMinTerminalFontSize = 8;
static constexpr int kMaxTerminalFontSize = 32;

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
    _fm = new QFontMetricsF(_font);
    recalculateCellSize();
    resetGlyphAtlas();
    _renderScheduler = new NovaTerm::RenderScheduler(this);
    _renderScheduler->setViewport(_core->columns(), _core->rows());
    connect(_renderScheduler, &NovaTerm::RenderScheduler::frameRequested,
            this,
            [this](const QVector<NovaTerm::DirtyRegion>& regions,
                   bool fullFrame,
                   bool overlayDirty) {
        const QMutexLocker lock(&_pendingFrameMutex);
        if (fullFrame) {
            _pendingDirtyRegions.clear();
            _fullFramePending = true;
        } else if (!_fullFramePending) {
            _pendingDirtyRegions += regions;
        }
        _overlayPending = _overlayPending || overlayDirty;
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

    // ── 连接 TerminalCore 信号 ───────────────────────────────
    connect(_core, &TerminalCore::damage, this,
            [this](const NovaTerm::DirtyRegion& region) {
        _renderScheduler->setViewport(_core->columns(), _core->rows());
        NovaTerm::DirtyRegion visible = region;
        visible.startRow += _scrollLine;
        visible.endRow += _scrollLine;
        _renderScheduler->schedule(visible);
    });

    connect(_core, &TerminalCore::cursorMoved, this, [this]() {
        requestOverlayFrame();
    });

    connect(_core, &TerminalCore::scrollbackChanged, this, [this]() {
        const int historyCount = _core->scrollbackLineCount();
        const int clampedOffset = std::clamp(_scrollLine, 0, historyCount);
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
        if (viewportMappingChanged)
            requestFullFrame();
        else if (selectionChanged)
            requestOverlayFrame();
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
    return result;
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
        requestFullFrame();
    }
}

void TerminalRenderer::scrollToLine(int line)
{
    const int maxScroll = _core->scrollbackLineCount();
    const int clamped = std::max(0, std::min(line, maxScroll));
    if (clamped != _scrollLine) {
        _scrollLine = clamped;
        requestFullFrame();
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
                // Scrollback 区域
                const int sbLine = _core->scrollbackLineCount() + row;  // row 是负值
                NovaTerm::Cell sc;
                if (!_core->getScrollbackCell(sbLine, col, sc)) {
                    result += QLatin1Char(' ');
                } else if (sc.isWideContinuation()) {
                    continue;
                } else if (sc.chars[0]) {
                    result += cellCharsToString(sc.chars.data(),
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
    {
        const QMutexLocker lock(&_pendingFrameMutex);
        pendingDirtyRegions.swap(_pendingDirtyRegions);
        fullFramePending = std::exchange(_fullFramePending, false);
        overlayPending = std::exchange(_overlayPending, false);
    }

    const bool contentPending = fullFramePending
        || !pendingDirtyRegions.isEmpty()
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
        fullFramePending = true;
        overlayPending = true;
    }

    QVector<bool> dirtyRows(rows, fullFramePending);
    if (!fullFramePending) {
        for (const NovaTerm::DirtyRegion& region : std::as_const(pendingDirtyRegions)) {
            const int start = std::clamp(region.startRow, 0, rows);
            const int end = std::clamp(region.endRow, 0, rows);
            for (int row = start; row < end; ++row)
                dirtyRows[row] = true;
        }
    }

    NovaTerm::RendererSnapshot screen;
    if (contentPending) {
        screen = _core->rendererSnapshot(dirtyRows, _scrollLine);
        if (screen.rows != rows || screen.columns != columns) {
            rows = screen.rows;
            columns = screen.columns;
            _commandBuffer.resize(rows, columns);
            dirtyRows.fill(true, rows);
            fullFramePending = true;
            overlayPending = true;
            screen = _core->rendererSnapshot(dirtyRows, _scrollLine);
        }
    }

    QElapsedTimer commandTimer;
    commandTimer.start();
    quint64 commandsGenerated = 0;
    const bool atlasResetDuringBuild = contentPending
        ? rebuildCommandRows(screen, dirtyRows, commandsGenerated) : false;
    if (atlasResetDuringBuild) {
        dirtyRows.fill(true);
        fullFramePending = true;
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
    uploadCommands(resourceUpdates, pixelSize, dirtyRows, bufferReallocated,
                   overlayPending || fullFramePending || bufferReallocated);
    if (_atlasDirty) {
        resourceUpdates->uploadTexture(_atlasTexture.get(), _atlasImage);
        _atlasDirty = false;
    }

    cb->beginPass(renderTarget(), _scheme.background, {1.0f, 0}, resourceUpdates);
    cb->setGraphicsPipeline(_pipeline.get());
    cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));
    cb->setShaderResources(_srb.get());

    for (int row = 0; row < rows; ++row) {
        const int vertexCount =
            int(_commandBuffer.row(row).backgrounds.size()) * 6;
        if (vertexCount <= 0)
            continue;
        const quint32 offset = quint32(row * _backgroundRowStrideVertices
                                      * int(sizeof(GpuVertex)));
        const QRhiCommandBuffer::VertexInput binding(_vertexBuffer.get(), offset);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(vertexCount);
        ++_renderStatistics.drawCalls;
    }
    const int contentBase = rows * _backgroundRowStrideVertices;
    for (int row = 0; row < rows; ++row) {
        const int vertexCount =
            int(_commandBuffer.row(row).contents.size()) * 6;
        if (vertexCount <= 0)
            continue;
        const quint32 offset = quint32(
            (contentBase + row * _contentRowStrideVertices)
            * int(sizeof(GpuVertex)));
        const QRhiCommandBuffer::VertexInput binding(_vertexBuffer.get(), offset);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(vertexCount);
        ++_renderStatistics.drawCalls;
    }
    if (_overlayVertexCount > 0) {
        const quint32 offset = quint32(_overlayBaseVertex
                                      * int(sizeof(GpuVertex)));
        const QRhiCommandBuffer::VertexInput binding(_vertexBuffer.get(), offset);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(_overlayVertexCount);
        ++_renderStatistics.drawCalls;
    }
    cb->endPass();

    _renderStatistics.cpuFrameNanoseconds += quint64(frameTimer.nsecsElapsed());
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
    if (_renderScheduler)
        _renderScheduler->setViewport(_core->columns(), _core->rows());
    requestFullFrame();
}

void TerminalRenderer::resizeTerminalToViewport()
{
    const int cols = qFloor(width()  / std::max<qreal>(1.0, _cellWidth));
    const int rows = qFloor(height() / std::max<qreal>(1.0, _cellHeight));

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
        emit terminalSizeChanged();
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
        if (pos.row < -_core->scrollbackLineCount())
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
    _vertexBufferSize = 0;
    {
        const QMutexLocker lock(&_pendingFrameMutex);
        _fullFramePending = true;
        _overlayPending = true;
    }
    _sampler.reset();
    _atlasTexture.reset();
}

void TerminalRenderer::resetGlyphAtlas()
{
    constexpr int kAtlasSize = 2048;
    _atlasImage = QImage(kAtlasSize, kAtlasSize, QImage::Format_RGBA8888);
    _atlasImage.fill(Qt::transparent);
    _atlasImage.setPixelColor(0, 0, Qt::white);
    _glyphs.clear();
    _atlasX = 1;
    _atlasY = 1;
    _atlasRowHeight = 0;
    _atlasDpr = devicePixelRatioF();
    _atlasDirty = true;
    ++_atlasGeneration;
}

void TerminalRenderer::ensureAtlasTexture()
{
    if (!_rhi)
        return;
    if (_atlasImage.isNull() || !qFuzzyCompare(_atlasDpr, devicePixelRatioF())) {
        resetGlyphAtlas();
        // Cached glyph commands refer to atlas pixels generated at the old
        // DPR. Rebuild every row before drawing against the new atlas.
        const QMutexLocker lock(&_pendingFrameMutex);
        _fullFramePending = true;
        _overlayPending = true;
    }
    if (_atlasTexture)
        return;

    _atlasTexture.reset(_rhi->newTexture(QRhiTexture::RGBA8, _atlasImage.size()));
    if (!_atlasTexture->create()) {
        qWarning() << "TerminalRenderer: failed to create glyph atlas texture";
        _atlasTexture.reset();
        return;
    }
    _atlasDirty = true;
}

void TerminalRenderer::ensurePipeline()
{
    if (_pipeline || !_rhi || !_atlasTexture)
        return;

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
                                                  _sampler.get())
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
    inputLayout.setBindings({{8 * int(sizeof(float))}});
    inputLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * int(sizeof(float))},
        {0, 2, QRhiVertexInputAttribute::Float4, 4 * int(sizeof(float))}
    });
    _pipeline->setVertexInputLayout(inputLayout);
    _pipeline->setShaderResourceBindings(_srb.get());
    _pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    _pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
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

const TerminalRenderer::GlyphEntry& TerminalRenderer::ensureGlyph(
    const QString& text, bool bold, int cellSpan)
{
    const QString key = QString::number(bold ? 1 : 0) + QLatin1Char(':')
        + QString::number(cellSpan) + QLatin1Char(':') + text;
    auto found = _glyphs.constFind(key);
    if (found != _glyphs.constEnd())
        return found.value();

    const qreal dpr = devicePixelRatioF();
    QFont glyphFont = _font;
    glyphFont.setBold(bold);
    const QFontMetricsF glyphMetrics(glyphFont);
    const qreal advance = glyphMetrics.horizontalAdvance(text);
    const qreal cellAdvance = _cellWidth * std::max(1, cellSpan);
    // Atlas allocation follows typographic advance and line metrics. Using
    // boundingRect() here would make each glyph quad depend on its visual ink
    // width and break the fixed terminal grid.
    const QRectF logicalRect(0.0, -glyphMetrics.ascent(),
                             std::max(advance, cellAdvance),
                             glyphMetrics.height());
    const QSize pixelSize(qCeil(logicalRect.width() * dpr),
                          qCeil(logicalRect.height() * dpr));
    const QRectF textureLogicalRect(
        QPointF(0.0, 0.0),
        QSizeF(pixelSize.width() / dpr, pixelSize.height() / dpr));
    const int paddedWidth = pixelSize.width() + 2;
    const int paddedHeight = pixelSize.height() + 2;
    if (_atlasX + paddedWidth > _atlasImage.width()) {
        _atlasX = 1;
        _atlasY += _atlasRowHeight;
        _atlasRowHeight = 0;
    }
    if (_atlasY + paddedHeight > _atlasImage.height())
        resetGlyphAtlas();

    // Paint onto an opaque RGB surface so Windows/Qt can produce sub-pixel
    // antialiasing. Transparent images only receive grayscale antialiasing.
    QImage rgbGlyphImage(pixelSize, QImage::Format_RGB32);
    rgbGlyphImage.fill(Qt::black);
    rgbGlyphImage.setDevicePixelRatio(dpr);
    QPainter painter(&rgbGlyphImage);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(glyphFont);
    painter.setPen(Qt::white);
    painter.drawText(QPointF(-logicalRect.left(),
                             glyphMetrics.ascent()), text);
    painter.end();
    const QImage glyphImage = alphaCoverageFromRgb(rgbGlyphImage, dpr);

    const QRect pixelRect(_atlasX + 1, _atlasY + 1,
                          pixelSize.width(), pixelSize.height());
    QPainter atlasPainter(&_atlasImage);
    atlasPainter.setCompositionMode(QPainter::CompositionMode_Source);
    // The glyph image carries a device-pixel ratio, whereas the atlas is a
    // raw DPR-1 GPU texture. Explicit rectangles avoid an implicit HiDPI
    // downscale followed by a GPU upscale, which makes glyphs look blurry.
    atlasPainter.drawImage(pixelRect, glyphImage, glyphImage.rect());
    atlasPainter.end();

    _atlasX += paddedWidth;
    _atlasRowHeight = std::max(_atlasRowHeight, paddedHeight);
    _atlasDirty = true;
    // Use the exact physical texture extent when building the GPU quad. This
    // prevents fractional DPR values from rescaling a ceil-rounded glyph by a
    // fraction of a pixel.
    return _glyphs.insert(key, GlyphEntry{pixelRect, textureLogicalRect}).value();
}

void TerminalRenderer::appendQuad(const QRectF& rect, const QRectF& uvRect,
                                  const QColor& color, const QSize& pixelSize)
{
    const qreal dpr = devicePixelRatioF();
    const float left = float(rect.left() * dpr / pixelSize.width() * 2.0 - 1.0);
    const float right = float(rect.right() * dpr / pixelSize.width() * 2.0 - 1.0);
    const float top = float(1.0 - rect.top() * dpr / pixelSize.height() * 2.0);
    const float bottom = float(1.0 - rect.bottom() * dpr / pixelSize.height() * 2.0);
    const float red = color.redF();
    const float green = color.greenF();
    const float blue = color.blueF();
    const float alpha = color.alphaF();
    const GpuVertex topLeft{left, top, float(uvRect.left()), float(uvRect.top()),
                            red, green, blue, alpha};
    const GpuVertex topRight{right, top, float(uvRect.right()), float(uvRect.top()),
                             red, green, blue, alpha};
    const GpuVertex bottomLeft{left, bottom, float(uvRect.left()), float(uvRect.bottom()),
                               red, green, blue, alpha};
    const GpuVertex bottomRight{right, bottom, float(uvRect.right()), float(uvRect.bottom()),
                                red, green, blue, alpha};
    _vertices << topLeft << bottomLeft << topRight
              << topRight << bottomLeft << bottomRight;
}

void TerminalRenderer::appendSolidRect(const QRectF& rect, const QColor& color,
                                       const QSize& pixelSize)
{
    const qreal atlasWidth = _atlasImage.width();
    const qreal atlasHeight = _atlasImage.height();
    appendQuad(rect,
               QRectF(0.25 / atlasWidth, 0.25 / atlasHeight,
                      0.5 / atlasWidth, 0.5 / atlasHeight),
               color, pixelSize);
}

void TerminalRenderer::appendTexturedRect(const QRectF& rect, const QRect& atlasRect,
                                          const QColor& color, const QSize& pixelSize)
{
    const qreal atlasWidth = _atlasImage.width();
    const qreal atlasHeight = _atlasImage.height();
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
    const qreal atlasWidth = _atlasImage.width();
    const qreal atlasHeight = _atlasImage.height();
    return {
        type,
        rect,
        QRectF(0.25 / atlasWidth, 0.25 / atlasHeight,
               0.5 / atlasWidth, 0.5 / atlasHeight),
        color
    };
}

void TerminalRenderer::requestFullFrame()
{
    if (_renderScheduler)
        _renderScheduler->scheduleFullFrame();
    else {
        {
            const QMutexLocker lock(&_pendingFrameMutex);
            _fullFramePending = true;
            _overlayPending = true;
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
    QVector<NovaTerm::RenderCommand>& backgrounds,
    QVector<NovaTerm::RenderCommand>& contents)
{
    const int cellSpan = std::max(1, static_cast<int>(cell.width));
    const qreal paintWidth = _cellWidth * cellSpan;
    QColor foreground = terminalColorToQColor(cell.foreground, true);
    QColor background = terminalColorToQColor(cell.background, false);
    if (cell.attributes.reverse)
        std::swap(foreground, background);
    backgrounds.push_back(makeSolidCommand(
        NovaTerm::RenderCommandType::BackgroundRect,
        QRectF(x, y, paintWidth, _cellHeight), background));

    if (cell.attributes.conceal)
        return;
    if (cell.chars[0] != 0 && cell.chars[0] != ' ') {
        const QString text = cellCharsToString(cell.chars.data(),
                                               NovaTerm::MaxCharsPerCell);
        if (!text.isEmpty()) {
            const auto& glyph =
                ensureGlyph(text, cell.attributes.bold, cellSpan);
            const qreal atlasWidth = _atlasImage.width();
            const qreal atlasHeight = _atlasImage.height();
            contents.push_back({
                NovaTerm::RenderCommandType::GlyphInstance,
                glyph.logicalRect.translated(x, y),
                QRectF(glyph.pixelRect.left() / atlasWidth,
                       glyph.pixelRect.top() / atlasHeight,
                       glyph.pixelRect.width() / atlasWidth,
                       glyph.pixelRect.height() / atlasHeight),
                foreground
            });
        }
    }
    if (cell.attributes.underline) {
        const qreal underlineY = y + _fm->ascent() + 2;
        contents.push_back(makeSolidCommand(
            NovaTerm::RenderCommandType::Underline,
            QRectF(x, underlineY, paintWidth, 1), foreground));
        if (cell.attributes.underlineStyle == NovaTerm::UnderlineStyle::Double)
            contents.push_back(makeSolidCommand(
                NovaTerm::RenderCommandType::Underline,
                QRectF(x, underlineY + 2, paintWidth, 1), foreground));
    }
    if (cell.attributes.strike)
        contents.push_back(makeSolidCommand(
            NovaTerm::RenderCommandType::Strike,
            QRectF(x, y + _cellHeight / 2, paintWidth, 1), foreground));
}

bool TerminalRenderer::rebuildCommandRows(
    const NovaTerm::RendererSnapshot& screen,
    const QVector<bool>& dirtyRows,
    quint64& commandsGenerated)
{
    bool rebuildAll = false;
    bool atlasReset = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const quint64 generationBefore = _atlasGeneration;
        for (int row = 0; row < dirtyRows.size(); ++row) {
            if (!rebuildAll && !dirtyRows[row])
                continue;
            rebuildCommandRow(row, screen);
            const auto& commands = _commandBuffer.row(row);
            commandsGenerated += quint64(commands.backgrounds.size()
                                         + commands.contents.size());
            ++_renderStatistics.rowsRebuilt;
        }
        if (_atlasGeneration == generationBefore)
            return atlasReset;
        atlasReset = true;
        rebuildAll = true;
    }
    qWarning() << "TerminalRenderer: glyph atlas repeatedly overflowed while rebuilding the visible grid";
    return true;
}

void TerminalRenderer::rebuildCommandRow(
    int widgetRow,
    const NovaTerm::RendererSnapshot& screen)
{
    QVector<NovaTerm::RenderCommand> backgrounds;
    QVector<NovaTerm::RenderCommand> contents;
    backgrounds.reserve(screen.columns);
    contents.reserve(screen.columns);

    for (int column = 0; column < screen.columns; ++column) {
        const qreal x = column * _cellWidth;
        const qreal y = widgetRow * _cellHeight;
        const NovaTerm::Cell* cell = screen.cellAt(widgetRow, column);
        if (cell && !cell->isWideContinuation())
            appendCellCommands(x, y, *cell, backgrounds, contents);
    }
    _commandBuffer.replaceRow(widgetRow, std::move(backgrounds),
                              std::move(contents));
}

quint64 TerminalRenderer::rebuildOverlays(
    const NovaTerm::CursorState& cursor)
{
    QVector<NovaTerm::RenderCommand> overlays;
    overlays.reserve(_core->rows() + 1);
    appendSelectionCommands(overlays);
    appendCursorCommand(overlays, cursor);
    const quint64 commandCount = quint64(overlays.size());
    _commandBuffer.replaceOverlays(std::move(overlays));
    return commandCount;
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
    int backgroundStride = columns * 6;
    int contentStride = columns * 24;
    for (int row = 0; row < _commandBuffer.rows(); ++row) {
        backgroundStride = std::max(
            backgroundStride,
            int(_commandBuffer.row(row).backgrounds.size()) * 6);
        contentStride = std::max(
            contentStride,
            int(_commandBuffer.row(row).contents.size()) * 6);
    }
    // Capacities only grow during a resource lifetime. A temporary complex
    // row must not move every following row back and forth between offsets.
    backgroundStride = std::max(backgroundStride,
                                _backgroundRowStrideVertices);
    contentStride = std::max(contentStride, _contentRowStrideVertices);
    const int overlayBase = rows * (backgroundStride + contentStride);
    const int overlayCapacity = std::max(
        std::max((rows + 2) * 6,
                 int(_commandBuffer.overlays().size()) * 6),
        _overlayCapacityVertices);
    const int requiredBytes =
        (overlayBase + overlayCapacity) * int(sizeof(GpuVertex));
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

    _vertexBuffer.reset();
    _vertexBufferSize = std::max(requiredBytes, 256 * 1024);
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
    return true;
}

void TerminalRenderer::uploadCommands(
    QRhiResourceUpdateBatch* updates,
    const QSize& pixelSize,
    const QVector<bool>& dirtyRows,
    bool uploadAllRows,
    bool overlayDirty)
{
    const int rows = _commandBuffer.rows();
    const int contentBase = rows * _backgroundRowStrideVertices;
    for (int row = 0; row < rows; ++row) {
        if (!uploadAllRows && !dirtyRows.value(row))
            continue;

        const NovaTerm::RenderCommandRow& commands = _commandBuffer.row(row);
        _vertices.clear();
        _vertices.reserve(commands.backgrounds.size() * 6);
        for (const NovaTerm::RenderCommand& command : commands.backgrounds)
            appendCommandVertices(command, pixelSize);
        Q_ASSERT(_vertices.size() <= _backgroundRowStrideVertices);
        if (!_vertices.isEmpty()) {
            const int bytes = int(_vertices.size() * sizeof(GpuVertex));
            const int offset = row * _backgroundRowStrideVertices
                * int(sizeof(GpuVertex));
            updates->updateDynamicBuffer(_vertexBuffer.get(), offset, bytes,
                                         _vertices.constData());
            _renderStatistics.gpuUploadBytes += quint64(bytes);
        }

        _vertices.clear();
        _vertices.reserve(commands.contents.size() * 6);
        for (const NovaTerm::RenderCommand& command : commands.contents)
            appendCommandVertices(command, pixelSize);
        Q_ASSERT(_vertices.size() <= _contentRowStrideVertices);
        if (!_vertices.isEmpty()) {
            const int bytes = int(_vertices.size() * sizeof(GpuVertex));
            const int offset =
                (contentBase + row * _contentRowStrideVertices)
                * int(sizeof(GpuVertex));
            updates->updateDynamicBuffer(_vertexBuffer.get(), offset, bytes,
                                         _vertices.constData());
            _renderStatistics.gpuUploadBytes += quint64(bytes);
        }
    }

    if (!overlayDirty)
        return;
    _vertices.clear();
    _vertices.reserve(_commandBuffer.overlays().size() * 6);
    for (const NovaTerm::RenderCommand& command : _commandBuffer.overlays())
        appendCommandVertices(command, pixelSize);
    _overlayVertexCount = _vertices.size();
    if (!_vertices.isEmpty()) {
        const int bytes = int(_vertices.size() * sizeof(GpuVertex));
        const int offset = _overlayBaseVertex * int(sizeof(GpuVertex));
        updates->updateDynamicBuffer(_vertexBuffer.get(), offset, bytes,
                                     _vertices.constData());
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
