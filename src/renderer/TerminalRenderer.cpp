#include "TerminalRenderer.h"
#include "core/terminal/TerminalCore.h"
#include <QPainter>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <QMouseEvent>
#include <QWheelEvent>
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

namespace {

// 把某行的脏列区间重置为"整行宽"。clear + push_back 而不是 `{0, columns}`
// 赋值：后者会新建一个一元素 QVector 并释放旧缓冲，整帧重建时每行一次堆
// 分配/释放。
void setFullRowSpan(QVector<NovaTerm::DirtyColumnSpan>& spans, int columns)
{
    spans.clear();
    spans.push_back({0, columns});
}

} // namespace

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
    _glyphRasterQueue.setReadyCallback([this] {
        QMetaObject::invokeMethod(this, [this] { requestOverlayFrame(); },
                                  Qt::QueuedConnection);
    });
    setApi(preferredRhiApi());

    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
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
    _renderScheduler->setViewport(_core->screenSize().first,
                                  _core->screenSize().second);
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
                // 视口尺寸改用命令缓冲的已知值：TerminalCore::rows()/columns()
                // 每次都要取 modelMutex，而解析线程最长持锁约 2.7 ms
                // （64 KiB 批次 @ 24 MiB/s），每个 damage 区域两次取锁会在
                // 持续输出时把 GUI 线程拖住。命令缓冲的尺寸每帧由
                // rendererSnapshot 同步，并在 render() 的尺寸不一致分支里
                // 回灌调度器，因此这里滞后一帧不影响裁剪正确性。
                const int rows = _commandBuffer.rows();
                const int columns = _commandBuffer.columns();
                if (rows > 0 && columns > 0)
                    _renderScheduler->setViewport(columns, rows);
                NovaTerm::DirtyRegion visible = region;
                visible.startRow += _scrollLine;
                visible.endRow += _scrollLine;
                _renderScheduler->schedule(visible, revision);
            });

    connect(_core, &TerminalCore::cursorMoved, this, [this]() {
        requestOverlayFrame();
    });

    connect(_core, &TerminalCore::scrollbackChanged, this, [this]() {
        // 布局维护被合并到"每个事件循环轮次一次"：
        //
        // updateHistoryLayout() 走 TerminalCore::scrollbackTail()，而该接口
        // 会把 [fromLineId .. lastLineId] 的逻辑行**深拷贝**一份（每行一个
        // Cell 向量）。解析线程按 64 KiB 一批发布，24 MiB/s 下约 380 次/秒，
        // 一次 200 列 × 80 行的批次约 830 KB 拷贝。布局只在渲染与命中测试
        // 时才被读，而渲染最多 60 Hz，因此没必要一个解析批次做一次。
        //
        // 做法：置脏标志 + 投递一次 queued 调用。事件队列里积压的多个
        // scrollbackChanged 会共用同一次维护（Qt 按序处理 posted 事件，
        // 同步调用排在它们之后）。GUI 比解析快时退化为每批一次 —— 与旧
        // 实现相同，不会更差。
        _historyLayoutDirty = true;
        if (_historyLayoutSyncScheduled)
            return;
        _historyLayoutSyncScheduled = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                _historyLayoutSyncScheduled = false;
                if (!_historyLayoutDirty)
                    return;
                _historyLayoutDirty = false;
                syncHistoryLayout();
            },
            Qt::QueuedConnection);
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
        if (batch.error.empty()) {
            if (batch.logicalStart == 0)
                _pendingHistoryLayout.clear();
            for (const NovaTerm::DisplayLine& displayLine : batch.rows)
                _pendingHistoryLayout.push_back(displayLine);
        } else {
            // 错误批次不携带行且 logicalStart 为 0，不能让它清掉已累积的
            // 结果。仍然继续提交已完成的部分 —— 旧写法直接返回，布局会
            // 永久缺失，行数于是长期退化成逻辑行数。
            qWarning() << "TerminalRenderer: 滚动历史重排失败："
                       << QString::fromStdString(batch.error);
        }
        if (!batch.completed)
            return;
        _historyLayout = std::move(_pendingHistoryLayout);
        _layoutColumns = _pendingLayoutColumns;
        if (_selectAllPending && _layoutColumns == _core->columns())
            selectAll();
        restoreScrollFromAnchor();
        publishScrollState();
        const bool selectionChanged = dropInvalidSelection();
        // 重排期间列宽可能又变了（例如仍在拖动窗口）。显式续排，避免在没有
        // 后续输出时布局停留在过期列宽上。
        if (_layoutColumns != _core->columns())
            _reflowDebounce->start();
        // Reflow changes only historical mapping. At the live bottom the
        // active screen identity and placement are unchanged, so rebuilding
        // base content would violate the mapping-only contract.
        if (_scrollLine > 0) {
            ++_viewportMappingRevision;
            requestFullFrame();
        } else if (selectionChanged) {
            requestOverlayFrame();
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
    // 前置过滤表只随规则集变化，重建一次即可。行文本侧每帧只做
    // O(字面量数) 的 contains 试探，跳过不可能命中的规则。
    _highlightPrefilters.clear();
    _highlightPrefilters.reserve(_highlightRules.size());
    _highlightNeedsFoldedText = false;
    for (const NovaTerm::TerminalHighlightRule& rule : _highlightRules) {
        HighlightPrefilter filter;
        filter.caseInsensitive =
            rule.pattern.patternOptions()
            & QRegularExpression::CaseInsensitiveOption;
        filter.literals = highlightRequiredLiterals(rule.pattern.pattern());
        _highlightNeedsFoldedText =
            _highlightNeedsFoldedText || filter.caseInsensitive;
        _highlightPrefilters.push_back(std::move(filter));
    }
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
    if (_scrollLine == 0)
        return;
    _scrollLine = 0;
    _scrollAnchorLine = 0;
    _scrollAnchorWrap = 0;
    ++_viewportMappingRevision;
    requestFullFrame();
    publishScrollState();
    // 布局常驻，回到实时底部不再丢弃它：行数始终可用于滚动条量程，再次
    // 进入历史也无需等待一次重建。
}

void TerminalRenderer::scrollToTop()
{
    scrollToLine(maximumScrollOffset());
}

bool TerminalRenderer::isModifierOnlyKey(int key)
{
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Shift:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
        return true;
    default:
        return false;
    }
}

int TerminalRenderer::maximumScrollOffset() const
{
    return _historyLayout.isEmpty()
        ? _core->scrollbackLineCount() : int(_historyLayout.size());
}

void TerminalRenderer::scrollToLine(int line)
{
    const int maxScroll = maximumScrollOffset();
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
        // 布局已常驻且与当前列宽一致，进入历史无需重排。
    }
    publishScrollState();
}

void TerminalRenderer::scrollLines(int delta)
{
    scrollToLine(_scrollLine + delta);
}

void TerminalRenderer::publishScrollState()
{
    const int maximumOffset = maximumScrollOffset();
    if (maximumOffset == _publishedMaximumOffset
        && _scrollLine == _publishedScrollOffset) {
        return;
    }
    _publishedMaximumOffset = maximumOffset;
    _publishedScrollOffset = _scrollLine;
    emit scrollStateChanged(maximumOffset, _scrollLine);
}

void TerminalRenderer::setConservativeLiveScrollRendering(bool enabled)
{
    // 保留入口但不再改变行为，见头文件说明：live-scroll 行槽位旋转快路径
    // 已删除（它在生产配置下不可达），因此没有可切换的第二种滚动策略。
    Q_UNUSED(enabled);
}

// ═══════════════════════════════════════════════════════════════════
//  选区
// ═══════════════════════════════════════════════════════════════════

bool TerminalRenderer::isRowContinuation(int documentRow) const
{
    if (documentRow < 0) {
        const qsizetype displayIndex = _historyLayout.size() + documentRow;
        if (displayIndex < 0 || displayIndex >= _historyLayout.size())
            return false;
        // wrapIndex > 0 表示该显示行是同一逻辑行内的后续折行。
        return _historyLayout[displayIndex].wrapIndex > 0;
    }
    return _core->rowContinuation(documentRow);
}

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

    // 整个提取过程复用同一份不可变快照。旧实现在行循环内逐行重取，既多余，
    // 又让不同行有可能读到不同版本的历史。
    const NovaTerm::ScrollbackSnapshot history =
        start.row < 0 ? _core->scrollbackSnapshot()
                      : NovaTerm::ScrollbackSnapshot{};

    QString result;
    for (int row = start.row; row <= end.row; ++row) {
        // 行间换行只在该行不是上一行的软换行延续时插入。超宽输出被自动换行
        // 成的多个显示行属于同一逻辑行，复制必须把它们拼回一行 —— 否则粘贴
        // 到编辑器里会断成多行。
        if (row > start.row && !isRowContinuation(row))
            result += QLatin1Char('\n');

        const int firstColumn = (row == start.row) ? start.col : 0;
        const int lastColumn =
            (row == end.row) ? end.col : _core->columns() - 1;

        // 历史行先解析出所属显示行与逻辑行，避免在列循环内重复查找。
        const NovaTerm::DisplayLine* display = nullptr;
        const NovaTerm::LogicalLine* logical = nullptr;
        if (row < 0) {
            const qsizetype displayIndex = _historyLayout.size() + row;
            if (displayIndex >= 0 && displayIndex < _historyLayout.size()) {
                display = &_historyLayout[displayIndex];
                logical = history.lineById(display->lineId);
            }
        }

        for (int column = firstColumn; column <= lastColumn; ++column) {
            const NovaTerm::Cell* source = nullptr;
            NovaTerm::Cell screenCell;
            if (row < 0) {
                if (!logical)
                    continue;
                const qsizetype cellIndex = display->startCell + column;
                // 超出该显示行实际持有的 Cell 范围时不补空格：补齐会把填充
                // 字符嵌进紧随其后被拼接的下一显示行之前。
                if (cellIndex >= display->endCell)
                    continue;
                source = &logical->cells[cellIndex];
            } else {
                if (!_core->getCell(row, column, screenCell))
                    continue;
                source = &screenCell;
            }

            // 宽字符延续格不产生字符，它属于前一个宽 Cell。
            if (source->isWideContinuation())
                continue;
            result += source->chars[0]
                ? cellCharsToString(source->chars.data(),
                                    NovaTerm::MaxCharsPerCell)
                : QString(QLatin1Char(' '));
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
    _selectAllPending = false;
    _selStart = {-1, -1};
    _selEnd   = {-1, -1};
    _selecting = false;
    requestOverlayFrame();
}

void TerminalRenderer::selectAll()
{
    const bool includeHistory = !_core->isAlternateScreen();
    if (includeHistory && _core->scrollbackLineCount() > 0
        && (_layoutColumns != _core->columns() || _historyLayout.isEmpty())) {
        // 使用现有分批重排，避免全选时在 GUI 线程遍历整份大历史。
        _selectAllPending = true;
        scheduleReflow();
        return;
    }
    _selectAllPending = false;
    _selecting = false;
    _selStart = {includeHistory ? -int(_historyLayout.size()) : 0, 0};
    const auto [selectCols, selectRows] = _core->screenSize();
    _selEnd = {selectRows - 1, selectCols - 1};
    requestOverlayFrame();
}

// ═══════════════════════════════════════════════════════════════════
//  坐标转换
// ═══════════════════════════════════════════════════════════════════

QPoint TerminalRenderer::widgetToCell(const QPoint& pos) const
{
    const auto [widgetCols, widgetRows] = _core->screenSize();
    const int col = std::clamp(qFloor(pos.x() / std::max<qreal>(1.0, _cellWidth)),
                               0, std::max(0, widgetCols - 1));
    const int widgetRow = qFloor(pos.y() / std::max<qreal>(1.0, _cellHeight));
    const int documentRow = std::clamp(widgetRow - _scrollLine,
                                       -_core->scrollbackLineCount(),
                                       std::max(0, widgetRows - 1));
    return QPoint(col, documentRow);
}

QPoint TerminalRenderer::widgetToScreenCell(const QPoint& pos) const
{
    const auto [hitCols, hitRows] = _core->screenSize();
    const int col = std::clamp(qFloor(pos.x() / std::max<qreal>(1.0, _cellWidth)),
                               0, std::max(0, hitCols - 1));
    const int row = std::clamp(qFloor(pos.y() / std::max<qreal>(1.0, _cellHeight)),
                               0, std::max(0, hitRows - 1));
    return QPoint(col, row);
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
    // ── 本函数内 TerminalCore 取锁情况（供后续优化参考）──────────
    // `rendererSnapshot()` 每帧取一次 modelMutex，这是无法回避的：渲染必须
    // 拿到一份一致快照（见 docs/ARCHITECTURE.md 的跨线程快照约束）。
    // 另外两处也取同一把锁：补回段在 `screen.revision > requestedContent
    // Revision` 时会**再取一次**；`cursorState()` 仅在 overlay-only 帧
    // （contentPending 为 false）取一次。damage 连接与各 overlay 追加函数
    // 原本也在逐行/逐匹配调 `rows()/columns()`，已全部改为用命令缓冲的
    // 尺寸（见 damage 连接与 rebuildOverlays 的注释）。
    //
    // 剩下的、真正的根治办法在 TerminalCore 侧：把尺寸与光标状态作为
    // 由解析 worker 更新的原子量发布出去，渲染器无锁读取 —— 与既有
    // `alternateScreenActive` / `mouseTrackingMode`（createAdapter() 里
    // 建立的缓存原子量）同一套做法。该改动属于 TerminalCore 的范围，
    // 本文件不越界修改。
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

    _glyphEnqueuedThisFrame = false;

    // 仅渲染线程访问 atlas/缓存；worker 只生成不可变 QImage 位图。
    bool glyphsReady = false;
    std::deque<NovaTerm::GlyphBitmap> readyBitmaps =
        _glyphRasterQueue.takeResults();
    for (const auto& bitmap : readyBitmaps) {
        if (bitmap.sourceGeneration != _fontManager.generation())
            continue;
        if (_glyphCache.insert(bitmap, _frameNumber))
            glyphsReady = true;
        ++_renderStatistics.glyphRasters;
    }
    _atlasGeneration = _glyphCache.atlas().generation();

    // Take ownership of this frame's requests up front. New requests arriving
    // while commands are built remain queued for the next frame instead of
    // invalidating this iteration or being erased at the end of this one.
    QVector<NovaTerm::DirtyRegion> pendingDirtyRegions;
    bool fullFramePending = false;
    bool overlayPending = false;
    int pendingLiveScrollRows = 0;
    quint64 requestedContentRevision = 0;
    {
        const QMutexLocker lock(&_pendingFrameMutex);
        pendingDirtyRegions.swap(_pendingDirtyRegions);
        fullFramePending = std::exchange(_fullFramePending, false);
        // requestFullFrame() and RenderScheduler::frameRequested() are
        // asynchronous. An already queued incremental QRhi frame can render
        // between them; retire the explicit-intent flag only together with an
        // actual full frame.
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
        const auto [snapCols, snapRows] = _core->screenSize();
        columns = snapCols;
        rows = snapRows;
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
    _glyphPendingRows.resize(std::size_t(rows), false);


    // 活动屏幕上滚只说明"有行进入了 scrollback"，并不能证明每一行保留的
    // GPU 行都能被同一次排列表示。旧实现在这里按 shell 类型二选一：光标
    // 定位重写的 shell（PowerShell/Clink）整屏重建，其余走行槽位旋转快路径。
    // 但该判据落在"可执行文件名是不是 wsl.exe"上，于是所有 POSIX shell、
    // cmd 与 PowerShell 全部落进重建分支 —— 快路径在生产中不可达，却留下
    // 一整套行身份哈希槽位分配（RowSlotMap::update）与其计数器。
    //
    // 判定所需的"这个 shell 是否发光标定位重写"是 LocalShellProfile 的能力
    // 位，宿主侧尚未提供（TerminalView 只能按可执行文件名 == wsl.exe 判断，
    // 见 setConservativeLiveScrollRendering 的说明），因此这里只保留已被
    // 四种传输 × 三个平台验证过的那一支；快路径连同 RowSlotMap::update
    // 一并删除，见 gpu/RowSlotMap.h 文件头。
    if (pendingLiveScrollRows > 0) {
        resetWidgetRowMapping(rows);
        fullFramePending = true;
        overlayPending = true;
        contentPending = true;
        pendingDirtyRegions.clear();
        ++_renderStatistics.revisionPromotedFullFrames;
    }

    if (glyphsReady) {
        for (int row = 0; row < rows; ++row) {
            if (_glyphPendingRows[std::size_t(row)]) {
                pendingDirtyRegions.push_back({row, row + 1, 0, columns});
                contentPending = true;
            }
        }
    }
    // 脏行标记与脏列区间写入跨帧复用的成员缓冲：旧写法每帧新建
    // `std::vector<bool>` 与 `QVector<QVector<...>>(rows)`，全帧时每行还要
    // 一次内层向量分配（Qt::QList 没有小缓冲优化）。内层向量用 clear +
    // push_back 而不是 `{0, columns}` 赋值，才能真正保住容量。
    std::vector<bool>& dirtyRows = _dirtyRowsScratch;
    QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans =
        _dirtySpansScratch;
    dirtyRows.assign(std::size_t(rows), fullFramePending);
    dirtySpans.resize(rows);
    for (int row = 0; row < rows; ++row) {
        if (fullFramePending)
            setFullRowSpan(dirtySpans[row], columns);
        else
            dirtySpans[row].clear();
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
        // 非脏行的内容指纹只在 row revision 超过本帧已投递的
        // requestedContentRevision 时才会被下方补回段读取，故把它作为门槛
        // 传入，让 core 跳过其余非脏行的整行哈希。
        screen = _core->rendererSnapshot(dirtyRows, _scrollLine,
                                         _scrollAnchorLine,
                                         _scrollAnchorWrap,
                                         requestedContentRevision);
        // The parser may publish another batch after its model lock is
        // released but before the queued damage signal reaches the GUI
        // thread. If this snapshot is newer than all damage delivered to the
        // scheduler, sparse rows are not a complete description of it.
        if (!fullFramePending
            && screen.revision > requestedContentRevision) {
            if (screen.visibleRowRevisions.size() == std::size_t(rows)
                && screen.visibleRowIdentities.size() == std::size_t(rows)) {
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
                    setFullRowSpan(dirtySpans[row], columns);
                }
                _renderStatistics.revisionRecoveredRows +=
                    quint64(recoveredRows);
                if (recoveredRows > 0) {
                    // 补回行必须重新拷贝 Cell，因此这第二次快照的 dirtyRows
                    // 与第一次相同 —— 不能只标补回行：第一次标脏的那些行在
                    // 新快照里同样只有 Cell 拷贝，不能沿用。
                    //
                    // 这里只做一处收窄：把 identity 门槛抬到
                    // requestedContentRevision，与第一次调用一致，于是 core
                    // 不必为"row revision 未前进的非脏活动行"重算指纹。真正
                    // 只取补回行的写法还要把第一次快照的 visibleRows /
                    // visibleRowBlockIdentities 逐行拼回来，收益是几十微秒的
                    // memcpy，而 modelMutex 的二次获取（解析线程最长持锁
                    // 2.7 ms）省不掉 —— 决定不取这个复杂度。
                    screen = _core->rendererSnapshot(
                        dirtyRows, _scrollLine, _scrollAnchorLine,
                        _scrollAnchorWrap, requestedContentRevision);
                }
            } else {
                std::fill(dirtyRows.begin(), dirtyRows.end(), true);
                for (int row = 0; row < rows; ++row)
                    setFullRowSpan(dirtySpans[row], columns);
                fullFramePending = true;
                overlayPending = true;
                ++_renderStatistics.revisionPromotedFullFrames;
                screen = _core->rendererSnapshot(
                    dirtyRows, _scrollLine, _scrollAnchorLine,
                    _scrollAnchorWrap, requestedContentRevision);
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
            dirtyRows.assign(std::size_t(rows), true);
            dirtySpans.resize(rows);
            for (int row = 0; row < rows; ++row)
                setFullRowSpan(dirtySpans[row], columns);
            fullFramePending = true;
            overlayPending = true;
            // 尺寸以快照为准，回灌调度器：damage 连接里改用命令缓冲的尺寸
            // 做裁剪判据（避免每个区域两次取 TerminalCore 的 modelMutex），
            // 尺寸变化必须在这里收敛，否则要等下一帧才更新裁剪范围。
            if (_renderScheduler)
                _renderScheduler->setViewport(columns, rows);
            screen = _core->rendererSnapshot(dirtyRows, _scrollLine,
                                             _scrollAnchorLine,
                                             _scrollAnchorWrap);
        }

        // ConPTY emits cursor-positioned fragments whose damage rectangle can
        // omit columns cleared later in the same parser publication. Compare
        // every dirty row with the renderer's actual 8-column block cache and
        // add only missing changed blocks rather than rebuilding every row.
        const std::vector<NovaTerm::u64> emptyBlockIdentities;
        for (int row = 0; row < rows; ++row) {
            if (!(row < int(dirtyRows.size()) && dirtyRows[row]))
                continue;
            const auto& blockIdentities =
                row < int(screen.visibleRowBlockIdentities.size())
                ? screen.visibleRowBlockIdentities[std::size_t(row)]
                : emptyBlockIdentities;
            dirtySpans[row] = _rowBlockDamageTracker.reconcileRow(
                row, blockIdentities, columns,
                std::move(dirtySpans[row]));
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
        std::fill(dirtyRows.begin(), dirtyRows.end(), true);
        for (int row = 0; row < rows; ++row)
            setFullRowSpan(dirtySpans[row], columns);
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
    // 栅格化队列深度只在"可能非零"时采样：size() 要在栅格化互斥锁下线性
    // 扫描整张 QHash(GlyphKey,bool>（上限 512 项），而这个计数器没有任何
    // 生产消费方（renderStatistics()/renderProgress() 只被 tests/ 与
    // tests/benchmarks/ 调用），每帧无条件扫描纯属浪费。
    //
    // 采样条件是精确的而非近似：队列只由 enqueue() 增长，所以
    // "上次深度为 0 且本帧既没入队（_glyphEnqueuedThisFrame）也没取走结果"
    // 必然仍是 0；深度非零时继续采样直到归零，因此 GPU 基准
    // "等 glyphRasterQueueDepth == 0" 的判据仍然可靠。
    if (_renderStatistics.glyphRasterQueueDepth > 0
        || _glyphEnqueuedThisFrame || !readyBitmaps.empty()) {
        _renderStatistics.glyphRasterQueueDepth =
            quint64(_glyphRasterQueue.size());
    }
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
    // 重排请求由 resizeTerminalToViewport 发起 —— 只有它知道列数是否真的变
    // 了。旧写法在此按 _scrollLine 分支，而 resizeTerminalToViewport 会先把
    // _scrollLine 清零，导致该分支在尺寸真变时永远不可达。
    resizeTerminalToViewport();
    if (_renderScheduler) {
        const auto [viewportCols, viewportRows] = _core->screenSize();
        _renderScheduler->setViewport(viewportCols, viewportRows);
    }
    requestFullFrame();
}

void TerminalRenderer::syncHistoryLayout()
{
    // 布局常驻：先增量维护到与快照一致，之后一切行数判断都用真实显示行
    // 数。旧实现在实时底部丢弃布局，使滚动条量程退化成逻辑行数。
    const int previousScroll = _scrollLine;
    updateHistoryLayout();
    restoreScrollFromAnchor();
    publishScrollState();
    const bool viewportMappingChanged =
        _scrollLine > 0 || _scrollLine != previousScroll;

    const bool selectionChanged = dropInvalidSelection();

    // At the live bottom, a scrollback append is accompanied by damage for
    // the active screen in the same parser publication. Scheduling a full
    // frame here would turn every output batch (especially shell/Clink
    // startup) into a complete CPU rebuild and GPU upload. A full rebuild
    // is only required while history rows are actually mapped into the
    // viewport or when clamping changed that mapping.
    if (viewportMappingChanged) {
        ++_viewportMappingRevision;
        requestFullFrame();
    } else if (selectionChanged) {
        requestOverlayFrame();
    }
}

void TerminalRenderer::updateHistoryLayout()
{
    const int columns = _core->columns();

    // 空历史：清掉残留布局，不触发重排。用 O(1) 的 lineCount 判断代替全量
    // 快照，等价原 history.empty() 快路径。
    if (_core->scrollbackLineCount() == 0) {
        // 清空历史后作废在途重排，防止迟到批次把旧历史重新装回布局。
        _reflowDebounce->stop();
        _core->cancelScrollbackReflow(_reflowGeneration);
        ++_reflowGeneration;
        _pendingHistoryLayout.clear();
        _historyLayout.clear();
        _layoutColumns = columns;
        if (_selectAllPending)
            selectAll();
        return;
    }
    // 列宽变化会让所有已有折点位移，增量维护无从下手；布局尚未建立时也不在
    // GUI 线程上折整个历史。两种情况都交给 worker 线程分批重排。
    if (columns != _layoutColumns || _historyLayout.isEmpty()) {
        scheduleReflow();
        return;
    }

    // ── 尾部增量：只取尾部若干逻辑行，不构造全量快照 ──
    // 每批 scrollbackChanged 都取全量 scrollbackSnapshot() 会 publish() →
    // sealActive()，把未填满的 active 块封存成小分块（碎片化），并复制全部
    // ChunkView。增量维护实际只需要首行 ID（头部淘汰）与尾部被改写/新增的
    // 逻辑行，故改用尾部增量窄接口。落后超过 kHistoryTailMax 时回退全量重排。
    constexpr NovaTerm::isize kHistoryTailMax = 4096;
    const NovaTerm::LineId lastLaidOut = _historyLayout.constLast().lineId;
    const NovaTerm::ScrollbackTail tail =
        _core->scrollbackTail(lastLaidOut, kHistoryTailMax);

    if (tail.lineCount == 0) {  // 历史在增量间隙被清空（clearScrollback）
        _historyLayout.clear();
        _layoutColumns = columns;
        return;
    }
    if (tail.resync) {  // sinceId 被头部淘汰或落后过远：交给全量重排
        scheduleReflow();
        return;
    }

    // ── 头部淘汰：首行 ID 前移说明老行已被逐出，删掉它们的显示行 ──
    qsizetype evicted = 0;
    while (evicted < _historyLayout.size()
           && _historyLayout[evicted].lineId < tail.firstLineId) {
        ++evicted;
    }
    if (evicted > 0)
        _historyLayout.remove(0, evicted);
    if (_historyLayout.isEmpty()) {
        // resync 已挡住 lastLaidOut < firstLineId 的情况，此处理论不可达；
        // 保守兜底，避免中间段缺失时错误增量。
        scheduleReflow();
        return;
    }

    // ── 尾部对齐 + 重折：删掉 lineId >= fromLineId 的显示行，再对 tail.lines
    //    逐条重折追加。这一步统一覆盖两种尾部变化：
    //    ① 尾条被 appendContinuation/sb_popline 原地改写（fromLineId ==
    //       lastLaidOut），丢弃旧显示行后按新内容重折；
    //    ② 尾行被 sb_popline 整条取回活动屏幕（屏幕变高）——被取回行的 id 全
    //       大于 fromLineId，会在此删除且不在 tail.lines 中，不会被重新加回。
    //    因此屏幕变高（行数变化）只做增量修正、不触发重排，与列宽变化区分开。──
    qsizetype tailStart = _historyLayout.size();
    while (tailStart > 0
           && _historyLayout[tailStart - 1].lineId >= tail.fromLineId) {
        --tailStart;
    }
    _historyLayout.remove(tailStart, _historyLayout.size() - tailStart);

    for (const NovaTerm::LogicalLine& line : tail.lines) {
        for (const NovaTerm::DisplayLine& displayLine :
             NovaTerm::LineLayout::wrapLine(line, columns))
            _historyLayout.push_back(displayLine);
    }
    ++_renderStatistics.historyLayoutTailUpdates;
    _layoutColumns = columns;
}

void TerminalRenderer::restoreScrollFromAnchor()
{
    // 重排在途时布局为空：按逻辑行数钳制（与 maximumScrollOffset() 同口径），
    // 而不是钳到 0。Ctrl+滚轮缩放会改变列宽并触发重排，钳到 0 会把回看位置
    // 直接丢回实时底部；保留偏移与锚点，重排完成后再由锚点精确还原。
    _scrollLine = std::clamp(_scrollLine, 0, maximumScrollOffset());
    if (_scrollLine <= 0 || _scrollAnchorLine == 0 || _historyLayout.isEmpty())
        return;
    // 锚点按 (逻辑行 ID, 折行序号) 定位，使折点变化后视口仍停在同一内容。
    for (qsizetype i = 0; i < _historyLayout.size(); ++i) {
        const NovaTerm::DisplayLine& row = _historyLayout[i];
        if (row.lineId == _scrollAnchorLine
            && row.wrapIndex == _scrollAnchorWrap) {
            _scrollLine = int(_historyLayout.size() - i);
            return;
        }
    }
}

bool TerminalRenderer::dropInvalidSelection()
{
    const bool hasSelectionState =
        _selecting || _selStart.col >= 0 || _selEnd.col >= 0;
    if (!hasSelectionState)
        return false;
    if (isDocumentPositionValid(_selStart)
        && isDocumentPositionValid(_selEnd)) {
        return false;
    }
    _selStart = {-1, -1};
    _selEnd = {-1, -1};
    _selecting = false;
    return true;
}

void TerminalRenderer::scheduleReflow()
{
    _reflowDebounce->stop();
    _core->cancelScrollbackReflow(_reflowGeneration);
    ++_reflowGeneration;
    ++_renderStatistics.scrollbackReflowRequests;
    _historyLayout.clear();
    _pendingHistoryLayout.clear();
    _layoutColumns = 0;
    _pendingLayoutColumns = _core->columns();
    _core->requestScrollbackReflow(_pendingLayoutColumns, _reflowGeneration,
                                   256);
    // 布局清空后量程回退到逻辑行数，外部滚动条需要同步这一过渡值。
    publishScrollState();
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

    const auto [knownCols, knownRows] = _core->screenSize();
    if (cols == knownCols && rows == knownRows)
        return;

    // 不再强制回到实时底部、也不再无条件清选区：正在翻看历史时改变列宽应当
    // 停在同一内容处，由 _scrollAnchorLine/_scrollAnchorWrap 在重排完成后还
    // 原（见 reflowBatchReady）。失效的选区在那里一并清除。
    const bool columnsChanged = cols != _core->columns();
    _core->resize(cols, rows);
    emit terminalSizeChanged(cols, rows);
    // 只有列数变化才需要重排 —— 行数变化不影响折行。
    if (columnsChanged)
        _reflowDebounce->start();
}

// ═══════════════════════════════════════════════════════════════════
//  键盘事件 → TerminalCore
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::keyPressEvent(QKeyEvent* event)
{
    emit activityDetected();

    // 有实际输入的按键才回到实时底部。单独按下的修饰键（Ctrl/Shift/Alt/
    // Meta 等）不产生输入，却是 Ctrl+滚轮缩放、Shift+滚轮本地回看等手势的
    // 起手式；让它们回到底部会把正在翻看的历史位置直接丢掉。
    if (_scrollLine > 0 && !isModifierOnlyKey(event->key()))
        scrollToBottom();

    _core->processKeyPress(event);
    event->accept();
}

void TerminalRenderer::inputMethodEvent(QInputMethodEvent* event)
{
    if (!event)
        return;

    emit activityDetected();
    if (_scrollLine > 0)
        scrollToBottom();

    if (!event->commitString().isEmpty())
        _core->processTextInput(event->commitString());
    event->accept();
}

QVariant TerminalRenderer::inputMethodQuery(
    Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle: {
        const NovaTerm::Position cursor = _core->cursorPosition();
        const QPoint position = cellToWidget(cursor.row, cursor.col);
        return QRectF(position.x(), position.y(), _cellWidth, _cellHeight);
    }
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return 0;
    case Qt::ImSurroundingText:
    case Qt::ImCurrentSelection:
        return QString{};
    default:
        return QRhiWidget::inputMethodQuery(query);
    }
}

// ═══════════════════════════════════════════════════════════════════
//  鼠标事件
// ═══════════════════════════════════════════════════════════════════

void TerminalRenderer::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        _selectAllPending = false;
    emit activityDetected();
    setFocus();

    // VT 鼠标跟踪开启时左键手势归应用：任何开启 ?1000/?1002/?1003 的 TUI
    // （vim、htop、opencode、claude 等）都自带选中与点击语义，本地选区
    // 让位；按住 Shift 强制走本地选区（xterm 逃生口，与各 TUI 自己的
    // "Shift+拖选走原生复制"提示语义一致）。
    // 判定只看按下时刻的跟踪状态并贯穿整个手势 —— 期间应用退出鼠标
    // 模式时，不能突然把半途的拖动切换成本地选区。
    const bool vtMouse =
        _core->mouseTrackingMode() != NovaTerm::MouseTrackingMode::None
        && !event->modifiers().testFlag(Qt::ShiftModifier);

    if (vtMouse) {
        _activeGestureIsVtMouse = true;
        _selecting = false;
        _autoCopyCurrentSelection = false;
        const QPoint screenCell = widgetToScreenCell(event->pos());
        _core->processMousePress(event, screenCell.y(), screenCell.x());
        event->accept();
        return;
    }
    _activeGestureIsVtMouse = false;

    // 普通终端才启用 NovaTerm 本地选择
    if (event->button() == Qt::LeftButton) {
        const QPoint cell = widgetToCell(event->pos());

        _selecting = true;
        _selStart = {cell.y(), cell.x()};
        _selEnd   = _selStart;

        _autoCopyCurrentSelection = true;

        requestOverlayFrame();
        event->accept();
        return;
    }

    event->accept();
}

void TerminalRenderer::mouseMoveEvent(QMouseEvent* event)
{
    // 本地选区手势进行中（Shift 逃生口或普通终端的左键拖动）时，
    // 移动事件归选区 —— 不能被 VT 鼠标分支抢走。
    if (_selecting) {
        const QPoint cell = widgetToCell(event->pos());
        _selEnd = {cell.y(), cell.x()};
        requestOverlayFrame();
        event->accept();
        return;
    }

    const auto mode = _core->mouseTrackingMode();

    // VT mouse：仅 Drag/Move 跟踪关心移动事件；Click 模式下转发只是
    // 徒增命令队列噪音（libvterm 内部也不会上报）。
    if (mode == NovaTerm::MouseTrackingMode::Drag ||
        mode == NovaTerm::MouseTrackingMode::Move) {
        const QPoint cell = widgetToScreenCell(event->pos());
        _core->processMouseMove(event, cell.y(), cell.x());
    }

    event->accept();
}

void TerminalRenderer::mouseReleaseEvent(QMouseEvent* event)
{
    // 左键手势以按下时刻的归属为准：VT 鼠标手势期间应用关闭跟踪
    // （如 TUI 退出）时，release 仍须送达应用而非落入本地选区分支；
    // 反过来本地选区的 release 也不能因跟踪中途开启而被改投应用。
    if (_activeGestureIsVtMouse) {
        _activeGestureIsVtMouse = false;
        _selecting = false;
        _autoCopyCurrentSelection = false;

        const QPoint screenCell = widgetToScreenCell(event->pos());
        _core->processMouseRelease(event, screenCell.y(), screenCell.x());
        event->accept();
        return;
    }

    if (_core->mouseTrackingMode() != NovaTerm::MouseTrackingMode::None) {
        // 非左键（中键/右键）没有选区手势，按当前跟踪状态转发。
        const QPoint screenCell = widgetToScreenCell(event->pos());
        _core->processMouseRelease(event, screenCell.y(), screenCell.x());
        event->accept();
        return;
    }

    // 普通终端：本地选择 + 自动复制
    if (_selecting && event->button() == Qt::LeftButton) {
        _selecting = false;

        const QPoint cell = widgetToCell(event->pos());
        _selEnd = {cell.y(), cell.x()};

        if (_autoCopyCurrentSelection && hasSelection())
            copySelection();

        _autoCopyCurrentSelection = false;

        requestOverlayFrame();
        event->accept();
        return;
    }

    event->accept();
}

void TerminalRenderer::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        _selectAllPending = false;
    // Qt 把双击的第二次按下投递为 doubleClick 而非 press；VT 跟踪开启时
    // 应用需要收到这次点击（双击选中是开启鼠标跟踪的 TUI 自带语义）。
    if (_core->mouseTrackingMode() != NovaTerm::MouseTrackingMode::None
        && !event->modifiers().testFlag(Qt::ShiftModifier)) {
        _activeGestureIsVtMouse = true;
        const QPoint screenCell = widgetToScreenCell(event->pos());
        _core->processMousePress(event, screenCell.y(), screenCell.x());
        event->accept();
        return;
    }

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
            _activeGestureIsVtMouse = false;
            _autoCopyCurrentSelection = false;
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
    const int notches = _wheelAccum / 120;  // 120 = 标准滚轮单位
    if (notches == 0) {
        event->accept();
        return;
    }
    _wheelAccum -= notches * 120;

    // 按住 Shift 强制走本地回看/选区 —— 鼠标上报/备用屏滚动期间的
    // 逃生口（xterm 惯例，各 TUI 也以此提示用户走终端原生交互）。
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);

    // VT 鼠标跟踪开启时，滚轮作为按键 4/5 上报给应用（开启鼠标模式的
    // vim、htop、less --mouse 等自己处理滚动）。
    if (!shift
        && _core->mouseTrackingMode() != NovaTerm::MouseTrackingMode::None) {
        const QPoint screenCell = widgetToScreenCell(event->position().toPoint());
        for (int n = 0, total = std::abs(notches); n < total; ++n) {
            _core->processWheel(notches > 0, screenCell.y(), screenCell.x(),
                                event->modifiers());
        }
        event->accept();
        return;
    }

    // 备用屏且无鼠标跟踪：Alternate Scroll（Windows Terminal 同款），
    // 把滚轮映射为 ↑/↓ 光标键，让 less / man 等可以滚动。
    if (!shift && _core->isAlternateScreen()) {
        _core->sendAlternateScroll(notches > 0,
                                   std::abs(notches) * kScrollWheelLines);
        event->accept();
        return;
    }

    // 正数 notches：向上滚动（回看历史），增加 _scrollLine
    // 负数 notches：向下滚动（返回底部），减少 _scrollLine
    scrollLines(notches * kScrollWheelLines);
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
    const auto [screenCols, screenRows] = _core->screenSize();
    if (pos.row < 0) {
        // scrollback 区域：row 从 -1（最新回滚行）到 -scrollbackLineCount（最旧）
        const qsizetype historyRows = _historyLayout.isEmpty()
            ? _core->scrollbackLineCount() : _historyLayout.size();
        if (pos.row < -historyRows)
            return false;
    } else {
        // 活跃屏幕：row 从 0 到 rows-1
        if (pos.row >= screenRows)
            return false;
    }
    if (pos.col < 0 || pos.col >= screenCols)
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
    // 彩色 emoji 全部位于 BMP 之外（U+1F000 以上），在 UTF-16 中必然表现为
    // 代理对；逐 QChar 扫描即可判定，避免旧实现每 Cell 一次 text.toUcs4()
    // 的堆分配。
    bool emoji = false;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar unit = text.at(index);
        if (!unit.isHighSurrogate())
            continue;
        const QChar low =
            index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (QChar::surrogateToUcs4(unit, low) >= 0x1f000) {
            emoji = true;
            break;
        }
    }
    // 一次字体选择同时得到 GlyphKey 与 FontSelection：命中缓存直接返回，
    // miss 时复用同一个 selection 入队栅格化，不再重复 select()。
    const auto keyAndSelection = _fontManager.makeKeyAndSelection(
        text, bold, false, cellSpan, devicePixelRatioF(),
        emoji ? NovaTerm::GlyphRenderMode::Color
              : NovaTerm::GlyphRenderMode::Grayscale);
    if (auto found = _glyphCache.find(keyAndSelection.key, _frameNumber))
        return *found;
    if (_buildingGlyphRow >= 0)
        _glyphPendingRows[std::size_t(_buildingGlyphRow)] = true;
    // 本帧"碰过"队列（无论入队成功还是命中去重）：队列深度需要重新采样。
    _glyphEnqueuedThisFrame = true;
    _glyphRasterQueue.enqueue({keyAndSelection.key,
                               keyAndSelection.selection.font, _cellWidth,
                               _cellHeight, true});
    // 未就绪时用空位置，避免把 solid glyph 当文字绘成实心方块。
    return {};
}

void TerminalRenderer::fillInstance(GpuInstance& out, const QRectF& rect,
                                    const QRectF& uvRect, const QColor& color)
{
    out.left = float(rect.left());
    out.top = float(rect.top());
    out.right = float(rect.right());
    out.bottom = float(rect.bottom());
    out.u0 = float(uvRect.left());
    out.v0 = float(uvRect.top());
    out.u1 = float(uvRect.right());
    out.v1 = float(uvRect.bottom());
    out.r = color.redF();
    out.g = color.greenF();
    out.b = color.blueF();
    out.a = color.alphaF();
    out.atlasPage = 0.0f;
    out.rowSlot = -1.0f;
    out.flags = 0.0f;
    out.reserved = 0.0f;
}

TerminalRenderer::GpuInstance TerminalRenderer::makeInstance(
    const QRectF& rect, const QRectF& uvRect, const QColor& color)
{
    GpuInstance instance{};
    fillInstance(instance, rect, uvRect, color);
    return instance;
}

TerminalRenderer::SpanInstances TerminalRenderer::assembleSpanInstances(
    const NovaTerm::RenderCommandRow& commands, int startColumn, int endColumn,
    int slot, QVector<GpuInstance>& backgroundScratch,
    QVector<GpuInstance>& contentScratch)
{
    SpanInstances counts;
    const int cellCount = std::max(0, endColumn - startColumn);
    if (cellCount == 0) {
        backgroundScratch.clear();
        contentScratch.clear();
        return counts;
    }
    counts.backgroundCount = cellCount;
    counts.contentCount = cellCount * 4;

    // 先各做一次 resize（内部至多一次 detach），随后热循环只走 data() 裸指针：
    // 逐元素 QVector::operator[] 在非 const 向量上每次都要做 detach 检查，
    // 是这段装配代码的主要开销之一。
    backgroundScratch.resize(cellCount);
    contentScratch.resize(counts.contentCount);
    GpuInstance* const backgroundOut = backgroundScratch.data();
    GpuInstance* const contentOut = contentScratch.data();

    // 背景：每列恰好一个实例。命令已按列升序，用游标线性推进逐列就地写入，
    // 不做"先零初始化局部变量再整块赋值"的双重 64 字节写；缺失命令的列写
    // 全零实例（退化四边形）。
    auto background = commands.backgrounds.cbegin();
    const auto backgroundEnd = commands.backgrounds.cend();
    for (int index = 0; index < cellCount; ++index) {
        const int column = startColumn + index;
        while (background != backgroundEnd && background->cellColumn < column)
            ++background;
        GpuInstance& target = backgroundOut[index];
        if (background != backgroundEnd && background->cellColumn == column) {
            // 同列重复命令（不应出现）与旧实现一致：后者覆盖前者。
            do {
                fillInstance(target, background->rect, background->uvRect,
                             background->color);
                target.atlasPage = float(background->atlasPage);
                target.rowSlot = float(slot);
                target.flags = background->colorGlyph ? 1.0f : 0.0f;
                ++background;
            } while (background != backgroundEnd
                     && background->cellColumn == column);
        } else {
            target = GpuInstance{};
        }
    }

    // 内容：每列 4 个槽位，单遍装配——先写实际命令占用的槽位，再在同一轮里把
    // 该格剩余槽位清零。旧实现先对整段 contentScratch 做一次 fill 再写第二遍，
    // 对 4 槽/Cell 的规模（200 列 ≈ 51 KB）两次扫描都会超出 L1，这里只扫一遍。
    auto content = commands.contents.cbegin();
    const auto contentEnd = commands.contents.cend();
    for (int index = 0; index < cellCount; ++index) {
        const int column = startColumn + index;
        while (content != contentEnd && content->cellColumn < column)
            ++content;
        GpuInstance* const cellOut = contentOut + index * 4;
        int ordinal = 0;
        while (content != contentEnd && content->cellColumn == column) {
            if (ordinal < 4) {
                GpuInstance& target = cellOut[ordinal];
                fillInstance(target, content->rect, content->uvRect,
                             content->color);
                target.atlasPage = float(content->atlasPage);
                target.rowSlot = float(slot);
                target.flags = content->colorGlyph ? 1.0f : 0.0f;
                ++ordinal;
            }
            // 第 5 条及以后与旧实现一样丢弃。
            ++content;
        }
        for (; ordinal < 4; ++ordinal)
            cellOut[ordinal] = GpuInstance{};
    }
    return counts;
}

void TerminalRenderer::appendQuad(const QRectF& rect, const QRectF& uvRect,
                                  const QColor& color, const QSize& pixelSize)
{
    Q_UNUSED(pixelSize);
    _instances.push_back(makeInstance(rect, uvRect, color));
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
                                               static_cast<quint64>(_core->modelRevision()));
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
            if (glyph.isValid())
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
    const std::vector<bool>& dirtyRows,
    const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
    quint64& commandsGenerated)
{
    const quint64 generationBefore = _atlasGeneration;
    for (int row = 0; row < int(dirtyRows.size()); ++row) {
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
    NovaTerm::RenderCommandRow& target =
        _commandBuffer.mutableRow(widgetRow);
    // 语义高亮色被烘焙进该行"默认色" Cell 的命令里。旧实现只要配置了规则就
    // 无条件 replaceAll，理由是"进出行的 token 会改变整行默认色 Cell 的颜色"
    // —— 但那只在**高亮角色本身发生变化**时成立。角色不变时未脏列沿用的旧
    // 命令仍带着同一个角色色，逐列增量重建是安全的；而绝大多数串口日志行不
    // 命中任何规则，角色恒为 NoHighlightRole，于是 1 列损坏过去也要重折
    // 200 列。改成按角色变化判定。
    const int role = rowHighlightRole(widgetRow, screen);
    const bool roleChanged = role != target.highlightRole;
    const bool replaceAll = roleChanged || dirtySpans.isEmpty()
        || (dirtySpans.size() == 1 && dirtySpans.front().startColumn <= 0
            && dirtySpans.front().endColumn >= screen.columns);
    _glyphPendingRows.resize(std::size_t(screen.rows), false);
    _buildingGlyphRow = widgetRow;
    if (replaceAll)
        _glyphPendingRows[std::size_t(widgetRow)] = false;

    // 就地重建：把目标行的旧命令 swap 到 scratch（只交换指针、不拷贝），目标
    // 向量随即变空但保留上一帧的容量，新命令可以直接写进去。背景仍按
    // 1 条/Cell 预留；内容层改按 1.5 条/Cell 起步——多数行只有字形
    // 1 条/Cell，装饰行最多 4 条/Cell（字形 + 双下划线 + 删除线），经
    // QVector 倍增自然长到位。增长只发生在行槽位生命周期的早期，容量
    // 随后跨帧保留，不形成每帧 realloc；按最坏 4× 预留会让 200 列视口
    // 白占约 4-7 MB。
    _oldBackgrounds.clear();
    _oldContents.clear();
    target.backgrounds.swap(_oldBackgrounds);
    target.contents.swap(_oldContents);
    if (target.backgrounds.capacity() < screen.columns)
        target.backgrounds.reserve(screen.columns);
    const qsizetype contentCapacity =
        screen.columns + qsizetype(screen.columns) / 2;
    if (target.contents.capacity() < contentCapacity)
        target.contents.reserve(contentCapacity);

    const std::optional<QColor> rowColor =
        role == NovaTerm::NoHighlightRole
        ? std::nullopt
        : std::optional<QColor>(
              highlightColor(NovaTerm::TerminalHighlightRole(role)));
    // 未脏列沿用上一帧命令、脏列重新生成，一次线性扫描完成（不需要排序）。
    NovaTerm::mergeRowCommandsIncremental(
        _oldBackgrounds, _oldContents, screen.columns, dirtySpans, replaceAll,
        [&](int column, QVector<NovaTerm::RenderCommand>& backgrounds,
            QVector<NovaTerm::RenderCommand>& contents) {
            const NovaTerm::Cell* cell = screen.cellAt(widgetRow, column);
            if (!cell || cell->isWideContinuation())
                return;
            appendCellCommands(column * _cellWidth, 0.0, *cell,
                               rowColor ? &*rowColor : nullptr,
                               backgrounds, contents);
        },
        target.backgrounds, target.contents);

    target.highlightRole = role;
    _buildingGlyphRow = -1;
    _commandBuffer.finishRow(widgetRow, _atlasGeneration);
    if (widgetRow >= 0 && widgetRow < _rowContentIdentities.size())
        _rowContentIdentities[widgetRow] =
            widgetRow < int(screen.visibleRowIdentities.size())
                ? screen.visibleRowIdentities[widgetRow] : quint64(0);
}

quint64 TerminalRenderer::rebuildOverlays(
    const NovaTerm::CursorState& cursor)
{
    // 尺寸取命令缓冲而非 TerminalCore::rows()/columns()：后两者每次都取
    // modelMutex（解析线程最长持锁约 2.7 ms），而 overlay 每帧都要重建。
    // 命令缓冲的尺寸每帧由 rendererSnapshot 同步，与 core 一致。
    const int rows = _commandBuffer.rows();
    const int columns = _commandBuffer.columns();
    // 写入复用的成员缓冲，再与命令缓冲交换：两侧容量都跨帧保留。旧写法
    // 每帧新建一个 rows+1 的 QVector 并把上一个 free 掉（50 行约 6 KB）。
    QVector<NovaTerm::RenderCommand>& overlays = _overlayScratch;
    overlays.clear();
    if (overlays.capacity() < rows + 1)
        overlays.reserve(rows + 1);
    appendSelectionCommands(overlays, rows, columns);
    appendSearchCommands(overlays, rows, columns);
    appendCursorCommand(overlays, cursor, rows, columns);
    const quint64 commandCount = quint64(overlays.size());
    _commandBuffer.swapOverlays(overlays);
    return commandCount;
}

void TerminalRenderer::setSearchMatches(
    std::vector<NovaTerm::SearchMatch> matches, quint64 generation)
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
    std::vector<NovaTerm::SearchMatch> matches, quint64 generation)
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
    QVector<NovaTerm::RenderCommand>& commands, int rows, int columns)
{
    if (_searchMatchesByLine.isEmpty() || _scrollLine <= 0)
        return;
    const QColor color(255, 196, 0, 105);
    if (!_historyLayout.isEmpty() && _scrollLine > 0) {
        const qsizetype first = std::max<qsizetype>(
            0, _historyLayout.size() - _scrollLine);
        const qsizetype last = std::min<qsizetype>(
            _historyLayout.size(), first + rows);
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
    // 布局尚未建立的回看视图：按快照把匹配折算到 widget 行。这条路径每帧
    // 只取一次快照（旧实现同样如此），但 rows/columns 必须来自入参 —— 循环
    // 内每个匹配各取一次 TerminalCore::rows()/columns() 会把 GUI 线程按在
    // 解析线程的 modelMutex 上，匹配数无上界。
    const auto history = _core->scrollbackSnapshot();
    for (auto it = _searchMatchesByLine.cbegin();
         it != _searchMatchesByLine.cend(); ++it) {
        const qsizetype documentRow = history.rowForLineId(it.key());
        if (documentRow < 0)
            continue;
        const int widgetRow = int(documentRow - history.lineCount())
            + _scrollLine;
        if (widgetRow < 0 || widgetRow >= rows)
            continue;
        for (const NovaTerm::SearchMatch& match : it.value()) {
            const qsizetype start = std::clamp<qsizetype>(
                match.startCell, 0, columns);
            const qsizetype end = std::clamp<qsizetype>(
                match.endCell, start, columns);
            commands.push_back(makeSolidCommand(
                NovaTerm::RenderCommandType::SearchOverlay,
                QRectF(start * _cellWidth, widgetRow * _cellHeight,
                       (end - start) * _cellWidth, _cellHeight), color));
        }
    }
}

void TerminalRenderer::appendCursorCommand(
    QVector<NovaTerm::RenderCommand>& commands,
    const NovaTerm::CursorState& cursor, int rows, int columns)
{
    if (!cursor.visible || _scrollLine != 0
        || (cursor.blink && !_cursorBlinkVisible))
        return;
    const auto position = cursor.position;
    if (position.row < 0 || position.row >= rows
        || position.col < 0 || position.col >= columns)
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
    QVector<NovaTerm::RenderCommand>& commands, int rows, int columns)
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
        if (widgetRow < 0 || widgetRow >= rows)
            continue;
        const int firstColumn = row == start.row ? start.col : 0;
        const int lastColumn = row == end.row ? end.col : columns - 1;
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

QVector<QString> TerminalRenderer::highlightRequiredLiterals(
    const QString& pattern)
{
    // 目标：找出"任何命中都必定包含"的字面量集合，用于在跑 PCRE2 之前廉价
    // 排除一行。只有一条硬约束：**绝不能排除任何一次真实命中**。因此每种
    // 被接受的形态都必须证明其元素是强制的（后面不跟 `?` / `*` 这类可选
    // 量词），否则一个空匹配就会绕过整个过滤。
    //
    // 认得的三种形态（其余一律返回空，调用方照旧执行正则）：
    //  1. 顶层零宽断言 (?=…) (?!…) (?<=…) (?<!…)：不产生匹配子串，跳过；
    //  2. 顶层字符组 [abc]（只含单字符、无区间与转义）且强制出现；
    //  3. 顶层非捕获组 (?:L1|L2|…)（各分支都是纯字面量）且强制出现。
    // 形如 \S*[>#$] 这类"中间夹着无法证明的结构"的模式拿不到过滤 —— 保守
    // 放弃比写一个半个正则解析器更可靠。
    QVector<QString> literals;
    // 从 open 处的 '(' 起找到配对的 ')'，返回其后的下标；找不到返回 -1。
    // 字符类里出现的括号会计错深度，此时返回错误结果并让调用方放弃过滤，
    // 不会漏掉命中。
    const auto groupEnd = [&pattern](int open) -> int {
        int depth = 0;
        for (int scan = open; scan < pattern.size(); ++scan) {
            const QChar unit = pattern.at(scan);
            if (unit == QLatin1Char('(')) {
                ++depth;
            } else if (unit == QLatin1Char(')')) {
                --depth;
                if (depth == 0)
                    return scan + 1;
            }
        }
        return -1;
    };
    // 从 text 的 from 处取一段纯字面量；遇到元字符即停，end 返回停点。
    // 必须显式收 text：分支字面量取自分组内部，不是原模式串的子串。
    const auto literalRun = [](const QString& text, int from, int& end) {
        static const QString metaCharacters =
            QStringLiteral("\\^$.|?*+()[]{}");
        int scan = from;
        while (scan < text.size()
               && !metaCharacters.contains(text.at(scan))) {
            ++scan;
        }
        end = scan;
        return text.mid(from, scan - from);
    };
    // 量词是否让紧邻的元素变成可选。`?` / `*` 一定可选；`{n,m}` 由下界
    // 决定；`+` 与无量词都强制。无法解析的量词按可选处理（保守）。
    const auto isMandatory = [&pattern](int after) {
        if (after >= pattern.size())
            return true;
        const QChar unit = pattern.at(after);
        if (unit == QLatin1Char('?') || unit == QLatin1Char('*'))
            return false;
        if (unit != QLatin1Char('{'))
            return true;  // 无量词或 '+'
        const int close = pattern.indexOf(QLatin1Char('}'), after);
        if (close < 0)
            return false;
        const QString body = pattern.mid(after + 1, close - after - 1);
        const int comma = body.indexOf(QLatin1Char(','));
        const QString minimum = comma < 0 ? body : body.left(comma);
        bool ok = false;
        return minimum.toInt(&ok) > 0 && ok;
    };

    int index = 0;
    while (index < pattern.size()) {
        const QChar unit = pattern.at(index);
        if (unit != QLatin1Char('(')) {
            // 顶层字符组。
            if (unit == QLatin1Char('[')) {
                const int close = pattern.indexOf(QLatin1Char(']'),
                                                  index + 1);
                if (close < 0)
                    return {};
                for (int scan = index + 1; scan < close; ++scan) {
                    const QChar member = pattern.at(scan);
                    if (member == QLatin1Char('-')
                        || member == QLatin1Char('^')
                        || member == QLatin1Char('\\')) {
                        return {};
                    }
                    literals.push_back(QString(member));
                }
                return isMandatory(close + 1) ? literals
                                               : QVector<QString>{};
            }
            // 其余只接受"整段就是字面量"的形态：后面还跟着别的东西就放弃。
            int end = index;
            const QString literal = literalRun(pattern, index, end);
            if (literal.isEmpty() || end != pattern.size())
                return {};
            literals = {literal};
            return literals;
        }

        // 零宽断言：(?=…) (?!…) (?<=…) (?<!…)。不产生匹配子串，跳过。
        if (index + 2 < pattern.size()
            && pattern.at(index + 1) == QLatin1Char('?')) {
            const QChar kind = pattern.at(index + 2);
            const bool lookbehind =
                kind == QLatin1Char('<')
                && index + 3 < pattern.size()
                && (pattern.at(index + 3) == QLatin1Char('=')
                    || pattern.at(index + 3) == QLatin1Char('!'));
            if (kind == QLatin1Char('=') || kind == QLatin1Char('!')
                || lookbehind) {
                const int end = groupEnd(index);
                if (end < 0)
                    return {};
                index = end;
                continue;
            }
            // 非捕获组 (?:…)，要求它内部是纯字面量的分支择一。
            if (kind == QLatin1Char(':')) {
                const int end = groupEnd(index);
                if (end < 0)
                    return {};
                const QString body = pattern.mid(index + 3,
                                                 end - 1 - (index + 3));
                const QStringList branches = body.split(QLatin1Char('|'));
                for (const QString& branch : branches) {
                    int branchEnd = 0;
                    const QString literal = literalRun(branch, 0, branchEnd);
                    if (literal.isEmpty() || branchEnd != branch.size())
                        return {};
                    literals.push_back(literal);
                }
                return isMandatory(end) ? literals : QVector<QString>{};
            }
        }
        // 其它分组 / 无 (? 标记的分组：无法证明，放弃。
        return {};
    }
    return literals;
}

int TerminalRenderer::rowHighlightRole(
    int widgetRow,
    const NovaTerm::RendererSnapshot& screen)
{
    if (_highlightRules.isEmpty())
        return NovaTerm::NoHighlightRole;

    // 行文本写入复用缓冲：旧实现为每个 Cell 调 cellCharsToString() 生成一
    // 个临时 QString（Qt::QList 没有小缓冲优化），200 列 × 50 行的全帧就是
    // 约 1 万次堆分配。逐字符追加与旧实现的 UTF-16 序列完全一致。
    _rowTextScratch.clear();
    if (_rowTextScratch.capacity() < screen.columns)
        _rowTextScratch.reserve(screen.columns);
    for (int column = 0; column < screen.columns; ++column) {
        const NovaTerm::Cell* cell = screen.cellAt(widgetRow, column);
        if (!cell || cell->isWideContinuation())
            continue;
        if (cell->chars[0] == 0)
            _rowTextScratch.append(QLatin1Char(' '));
        else
            appendCellChars(_rowTextScratch, cell->chars.data(),
                            NovaTerm::MaxCharsPerCell);
    }
    const QStringView text(_rowTextScratch);
    // 只有存在大小写不敏感规则时才需要小写副本；有则整行折叠一次（无堆
    // 分配），而不是让每次 contains 都走逐字符 toLower。
    if (_highlightNeedsFoldedText) {
        _rowTextFoldedScratch.resize(_rowTextScratch.size());
        const QChar* source = _rowTextScratch.constData();
        QChar* destination = _rowTextFoldedScratch.data();
        for (qsizetype index = 0; index < _rowTextScratch.size(); ++index)
            destination[index] = source[index].toLower();
    }
    const QStringView folded(_rowTextFoldedScratch);

    for (qsizetype index = 0; index < _highlightRules.size(); ++index) {
        const NovaTerm::TerminalHighlightRule& rule = _highlightRules[index];
        if (!rule.pattern.isValid())
            continue;
        // 前置过滤：字面量一个都不在行里，该正则绝无可能命中，直接跳过。
        const HighlightPrefilter& filter = _highlightPrefilters[index];
        if (!filter.literals.isEmpty()) {
            const QStringView haystack =
                filter.caseInsensitive ? folded : text;
            bool maybe = false;
            for (const QString& literal : filter.literals) {
                if (haystack.contains(literal,
                                      filter.caseInsensitive
                                      ? Qt::CaseInsensitive
                                      : Qt::CaseSensitive)) {
                    maybe = true;
                    break;
                }
            }
            if (!maybe)
                continue;
        }
        if (rule.pattern.matchView(text).hasMatch())
            return int(rule.role);
    }
    return NovaTerm::NoHighlightRole;
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
    // 复用成员缓冲：旧实现每帧构造一个 kPlacementFloatCount（1048 项，
    // 4192 字节）的零初始化 QVector。着色器对 `instanceMeta.y >=
    // viewport.w` 的槽位提前 return，根本不索引 rowPlacement[]，因此除
    // "[0, rowCount) 内的槽位"以外的内容无需每帧清零 —— 只写头部、修正
    // 矩阵与实际用到的槽位即可。
    if (_placementScratch.size() != kPlacementFloatCount)
        _placementScratch.resize(kPlacementFloatCount);
    float* values = _placementScratch.data();
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
                                 int(_placementScratch.size() * sizeof(float)),
                                 _placementScratch.constData());
    _renderStatistics.gpuUploadBytes +=
        quint64(_placementScratch.size() * sizeof(float));
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
    const std::vector<bool>& dirtyRows,
    const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
    bool uploadAllRows,
    bool overlayDirty)
{
    const int rows = _commandBuffer.rows();
    const int contentBase = rows * _backgroundRowStrideVertices;
    // 整行宽的列区间用一份复用的成员缓冲：旧写法在 uploadAllRows 时为**每一
    // 行**新建一个一元素 QVector（并顺带拷贝 dirtySpans[row]），全帧就是
    // 50 次堆分配。
    _fullRowSpanScratch.resize(1);
    _fullRowSpanScratch[0] = {0, _commandBuffer.columns()};
    for (int row = 0; row < rows; ++row) {
        if (!uploadAllRows
            && !(row < int(dirtyRows.size()) && dirtyRows[row]))
            continue;

        const NovaTerm::RenderCommandRow& commands = _commandBuffer.row(row);
        const int slot = _rowSlotMap.slotForWidgetRow(row);
        if (slot < 0)
            continue;
        // 取该行脏区间的引用，避免拷贝那个向量（Qt::QList 逐元素拷贝）。
        // 脏行却没有任何脏列区间（只有行级命令）时退回整行宽。
        const QVector<NovaTerm::DirtyColumnSpan>& rowDirtySpans =
            uploadAllRows ? _fullRowSpanScratch : dirtySpans.value(row);
        const QVector<NovaTerm::DirtyColumnSpan>& spans =
            rowDirtySpans.isEmpty() ? _fullRowSpanScratch : rowDirtySpans;
        for (const auto& rawSpan : std::as_const(spans)) {
            const int start = std::clamp(rawSpan.startColumn, 0,
                                         _commandBuffer.columns());
            const int end = std::clamp(rawSpan.endColumn, start,
                                       _commandBuffer.columns());
            if (start >= end)
                continue;
            const auto counts = assembleSpanInstances(
                commands, start, end, slot, _backgroundInstances,
                _contentInstances);

            // 上传范围仍按保留 stride 计算：全帧重建（uploadAllRows）时必须把
            // stride 尾部清零，否则缩窄终端后旧槽位会残留。实例数量已按 span
            // 收窄，只有 stride 大于本区间时才需要额外清零尾部。
            const int backgroundVertexCount = NovaTerm::rowUploadVertexCount(
                counts.backgroundCount, _backgroundRowStrideVertices,
                uploadAllRows);
            if (backgroundVertexCount > counts.backgroundCount) {
                _backgroundInstances.resize(backgroundVertexCount);
                std::fill(_backgroundInstances.begin()
                              + counts.backgroundCount,
                          _backgroundInstances.end(), GpuInstance{});
            }
            const int backgroundBytes =
                int(_backgroundInstances.size() * sizeof(GpuInstance));
            const int backgroundOffset =
                (slot * _backgroundRowStrideVertices + start)
                * int(sizeof(GpuInstance));
            updates->updateDynamicBuffer(_vertexBuffer.get(), backgroundOffset,
                                         backgroundBytes,
                                         _backgroundInstances.constData());
            _renderStatistics.gpuUploadBytes += quint64(backgroundBytes);
            // 注意：contentUploadBytes 的口径是"基础内容区域的上传字节"
            // （背景 + 内容两层），RendererP5GpuBenchmark 的保留 stride
            // 不变量按 5 实例/Cell 断言，不能只统计内容层。
            _renderStatistics.contentUploadBytes += quint64(backgroundBytes);

            const int contentVertexCount = NovaTerm::rowUploadVertexCount(
                counts.contentCount, _contentRowStrideVertices, uploadAllRows);
            if (contentVertexCount > counts.contentCount) {
                _contentInstances.resize(contentVertexCount);
                std::fill(_contentInstances.begin() + counts.contentCount,
                          _contentInstances.end(), GpuInstance{});
            }
            const int contentBytes =
                int(_contentInstances.size() * sizeof(GpuInstance));
            const int contentOffset =
                (contentBase + slot * _contentRowStrideVertices + start * 4)
                * int(sizeof(GpuInstance));
            updates->updateDynamicBuffer(_vertexBuffer.get(), contentOffset,
                                         contentBytes,
                                         _contentInstances.constData());
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
    const auto [selCols, visRows] = _core->screenSize();
    const int cols = selCols;
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
    const auto [cursorCols, cursorRows] = _core->screenSize();
    if (cpos.row < 0 || cpos.row >= cursorRows ||
        cpos.col < 0 || cpos.col >= cursorCols)
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

    // 提到循环外：原先每个选中行各问一次 rows() 与 columns()（D-016）。
    const auto [selCols, selRows] = _core->screenSize();
    const int lastColumn = selCols - 1;
    for (int row = start.row; row <= end.row; ++row) {
        const int widgetRow = row + _scrollLine;
        if (widgetRow < 0 || widgetRow >= selRows)
            continue;

        int c1 = (row == start.row) ? start.col : 0;
        int c2 = (row == end.row)   ? end.col   : lastColumn;

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
    appendCellChars(result, chars, maxCount);
    return result;
}

// 逐字符追加，避免为组合字符簇的每个 Cell 生成临时 QString。产出的 UTF-16
// 序列与旧实现完全一致：BMP 内直接单 QChar，补充平面走代理对。
void TerminalRenderer::appendCellChars(QString& out, const uint32_t* chars,
                                       int maxCount)
{
    for (int i = 0; i < maxCount && chars[i] != 0; ++i) {
        if (QChar::requiresSurrogates(chars[i])) {
            out += QChar::fromUcs4(chars[i]);
        } else {
            out += QChar(static_cast<ushort>(chars[i]));
        }
    }
}
