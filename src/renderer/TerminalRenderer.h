#pragma once
#include "HistoryLayout.h"
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
#include "TerminalHighlighting.h"
#include "font/FontManager.h"
#include "glyph/GlyphCache.h"
#include "glyph/GlyphRasterizer.h"
#include "gpu/BufferBudget.h"
#include "gpu/RendererCapabilities.h"
#include "gpu/RowSlotMap.h"
#include "RowBlockDamageTracker.h"
#include "ScrollDamageHandoff.h"

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
        /**
         * @brief 走尾部增量路径维护历史显示布局的次数。
         *
         * 每次都要调 `TerminalCore::scrollbackTail()`，它把尾部逻辑行深拷贝
         * 一份。解析按批次发布，该计数用于验证同一事件循环轮次里的多次发布
         * 被合并成一次（见 syncHistoryLayout）。
         */
        quint64 historyLayoutTailUpdates{0};
        quint64 capabilityFallbacks{0};
        quint64 viewportMappingRevision{0};
    };

    struct RenderProgress
    {
        quint64 rowsRebuilt{0};
        quint64 lastRenderedRevision{0};
        quint64 framesRendered{0};
    };

    explicit TerminalRenderer(TerminalCore* core, QWidget* parent = nullptr);
    ~TerminalRenderer() override;

    // ── 外观 ───────────────────────────────────────────────────
    void setColorScheme(const TerminalColorScheme& scheme);
    const TerminalColorScheme& colorScheme() const { return _scheme; }
    void setHighlightRules(
        QVector<NovaTerm::TerminalHighlightRule> rules);

    void setFont(const QFont& font);
    QFont font() const { return _font; }

    void zoomIn();
    void zoomOut();

    // ── 滚动 ───────────────────────────────────────────────────
    int scrollOffset() const { return _scrollLine; }

    /**
     * @brief 滚动历史在当前列宽下占用的显示行数。
     * @note  这就是滚动条量程。布局常驻后它始终是真实显示行数，不再退化成
     *        逻辑行数（逻辑行数在有折行时偏小）。
     */
    [[nodiscard]] qsizetype historyDisplayRowCount() const
    {
        return _historyLayout.size();
    }

    /**
     * @brief 当前允许的最大滚动偏移，即外部滚动条的量程上限。
     * @note  布局常驻时等于 historyDisplayRowCount()；全量重排进行中布局暂
     *        为空，此时回退到逻辑行数 —— 与 scrollToLine() 的钳制口径一致，
     *        滚动条不会在每次列宽变化时先塌缩到 0 再恢复。
     */
    [[nodiscard]] int maximumScrollOffset() const;
    /**
     * @brief 滚动到历史最顶部（偏移 = maximumScrollOffset()）。
     */
    void scrollToTop();
    void scrollToBottom();
    /**
     * @brief 判断按键是否为单独的修饰/锁定键（不产生终端输入）。
     * @param key Qt::Key 值。
     * @return 是修饰键时返回 true；此类按键不会把回看视图拉回实时底部。
     */
    [[nodiscard]] static bool isModifierOnlyKey(int key);
    void scrollToLine(int line);
    void scrollLines(int delta);

    /**
     * @brief 宿主声明本地 shell 是否需要"保守"的活动屏幕上滚渲染。
     *
     * @note 保留此入口只为兼容 `TerminalView::startLocalShell()` 的既有调用。
     *       它原先用来在"滚动时旋转行槽位环的快路径"与"每批滚动都重建最终
     *       快照"之间二选一，而快路径在所有生产配置下都不可达
     *       （`_conservativeLiveScrollRendering` 默认为 true，宿主对除
     *       wsl.exe 外的每个本地 shell 都传 true），已随该快路径一并删除。
     *       因此本函数当前不再改变任何渲染行为。
     *
     *       若要重新启用该快路径，判定依据（"这个 shell 是否发出光标定位
     *       重写"）必须来自 shell profile 而不是可执行文件名，且需要
     *       `LocalShellProfile` 增加一个能力位、在 `TerminalView` 里按该位
     *       调用本函数。参见 docs/architecture/stages/P5_Glyph_and_GPU_Pipeline.md
     *       的 live-scroll 行槽位一节。
     */
    void setConservativeLiveScrollRendering(bool enabled);

    // ── 选区 ───────────────────────────────────────────────────
    QString selectedText() const;
    bool hasSelection() const;
    void copySelection();
    /** @brief 选择完整历史与当前屏幕；备用屏仅选择当前屏幕。 */
    void selectAll();
    void clearSelection();

    // ── 从 widget 坐标计算 cell 坐标（供外部使用）─────────────
    QPoint widgetToCell(const QPoint& pos) const;
    // 与 widgetToCell 不同：返回屏幕行（0..rows-1），不随回看滚动偏移，
    // 供鼠标事件上报使用（终端协议上报的是可见屏幕坐标）。
    QPoint widgetToScreenCell(const QPoint& pos) const;
    RenderStatistics renderStatistics() const;
    RenderProgress renderProgress() const;
    void setTargetRefreshRate(int hz);
    void setSearchMatches(std::vector<NovaTerm::SearchMatch> matches,
                          quint64 generation);
    void appendSearchMatches(std::vector<NovaTerm::SearchMatch> matches,
                             quint64 generation);
    void clearSearchMatches();
    qsizetype searchMatchCount() const;
    qsizetype searchMatchedLineCount() const
    {
        return _searchMatchesByLine.size();
    }

signals:
    void activityDetected();
    void terminalSizeChanged(int columns, int rows);
    /**
     * @brief 滚动量程或当前偏移发生变化。
     * @param maximumOffset 允许的最大滚动偏移，即 maximumScrollOffset()。
     * @param offset        当前偏移，即 scrollOffset()；0 表示实时底部。
     * @note  仅在二者之一真正变化时发出，宿主据此同步外部滚动条即可，不必
     *        轮询。滚轮、键盘回底、历史追加/淘汰与重排完成都会经此发布。
     */
    void scrollStateChanged(int maximumOffset, int offset);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;

public:
    /// GPU 顶点实例：背景与内容共用的 16 个 float 布局（64 字节）。
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

    /// 一个列区间装配出的实例数量（单位：instance）。
    struct SpanInstances
    {
        int backgroundCount{0}; ///< 背景：每列 1 个
        int contentCount{0};    ///< 内容：每列 4 个槽位
    };

    /**
     * @brief 把一个列区间内的命令装配成 GpuInstance 序列（纯 CPU，可单测）。
     *
     * @param commands 该行命令；backgrounds/contents 必须按 cellColumn 升序
     *                 （`rebuildCommandRow()` 的 stable_sort 保证）。
     * @param startColumn 区间起始列（含）。
     * @param endColumn 区间结束列（不含）；调用方已裁剪到 [0, columns]。
     * @param slot 该行在 row slot 环中的槽位，写入实例的 rowSlot 字段。
     * @param backgroundScratch 输出：长度 == endColumn-startColumn，每列恰好
     *        一个实例；该列没有背景命令时写全零实例（退化为不绘制）。
     * @param contentScratch 输出：长度 == (endColumn-startColumn)*4；未被命令
     *        占用的槽位保持全零，避免顶点着色器画出上一帧的残留字形。
     * @return 两份 scratch 中实际需要上传的实例数量。
     * @note 只初始化本次上传范围：背景逐列直接覆盖（不预先填充整个区间），
     *       内容只清零本区间的 4 槽位，不触碰保留 stride 的尾部。
     */
    [[nodiscard]] static SpanInstances assembleSpanInstances(
        const NovaTerm::RenderCommandRow& commands, int startColumn,
        int endColumn, int slot, QVector<GpuInstance>& backgroundScratch,
        QVector<GpuInstance>& contentScratch);

    /**
     * @brief 从正则模式里提取"任何命中都必定包含"的字面量集合。
     *
     * 用于高亮规则的廉价前置过滤：集合为空表示无法证明（调用方必须照旧跑
     * 正则），非空时只有行文本包含其中任意一项才可能命中。
     *
     * 识别的形态（其余一律返回空，保持保守）：
     *  - 顶层零宽断言组（`(?<!…)` / `(?!…)` / `(?=…)` / `(?<=…)`）不产生
     *    文本，跳过；
     *  - 纯字面量段：取整段作为唯一必需字面量；
     *  - 顶层 `(?:a|b|c)` 且各分支均为纯字面量：取各分支；
     *  - 顶层字符组 `[abc]`（只含单字符，无区间/转义）：取各字符。
     *
     * @param pattern 正则模式串。
     * @return 必需字面量；无法证明时为空。
     */
    [[nodiscard]] static QVector<QString> highlightRequiredLiterals(
        const QString& pattern);

private:
    // ── 渲染辅助 ──────────────────────────────────────────────
    void recalculateCellSize();
    void resizeTerminalToViewport();
    QPoint cellToWidget(int documentRow, int col) const;
    int cellRowAt(int widgetY) const;
    int cellColAt(int widgetX) const;
    bool isDocumentPositionValid(const NovaTerm::Position& pos) const;

    /**
     * @brief 文档行是否为上一行的软换行延续。
     * @param documentRow 文档行号，负数表示滚动历史，非负表示活动屏幕。
     * @return true 表示该行续接上一行，其间没有真实换行。
     * @note  历史行用 DisplayLine::wrapIndex > 0 判定，活动屏幕行取
     *        TerminalCore::rowContinuation()。复制选区据此决定是否插入
     *        换行，使超宽行拼回单行。
     */
    [[nodiscard]] bool isRowContinuation(int documentRow) const;
    uint32_t documentCellCodepoint(int documentRow, int col) const;

    bool rebuildCommandRows(const NovaTerm::RendererSnapshot& screen,
                            const std::vector<bool>& dirtyRows,
                            const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
                            quint64& commandsGenerated);
    void rebuildCommandRow(int widgetRow,
                           const NovaTerm::RendererSnapshot& screen,
                           const QVector<NovaTerm::DirtyColumnSpan>& dirtySpans);
    quint64 rebuildOverlays(const NovaTerm::CursorState& cursor);
    void appendCellCommands(qreal x, qreal y, const NovaTerm::Cell& cell,
                            const QColor* defaultForegroundOverride,
                            QVector<NovaTerm::RenderCommand>& backgrounds,
                            QVector<NovaTerm::RenderCommand>& contents);
    void appendCursorCommand(QVector<NovaTerm::RenderCommand>& commands,
                             const NovaTerm::CursorState& cursor,
                             int rows, int columns);
    void appendSelectionCommands(QVector<NovaTerm::RenderCommand>& commands,
                                 int rows, int columns);
    void appendSearchCommands(QVector<NovaTerm::RenderCommand>& commands,
                              int rows, int columns);
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
    [[nodiscard]] static GpuInstance makeInstance(const QRectF& rect,
                                                  const QRectF& uvRect,
                                                  const QColor& color);
    /**
     * @brief 就地写入一条实例的几何与颜色字段。
     * @param out 目标实例（其余字段由调用方按需覆盖）。
     * @note  装配热路径专用：避免 `makeInstance()` 返回值再整体赋值带来的
     *        第二次 64 字节写入，也避免逐元素 `QVector::operator[]` 的
     *        detach 检查（调用方先取 `data()` 裸指针）。
     */
    static void fillInstance(GpuInstance& out, const QRectF& rect,
                             const QRectF& uvRect, const QColor& color);
    void appendQuad(const QRectF& rect, const QRectF& uvRect,
                    const QColor& color, const QSize& pixelSize);
    NovaTerm::GlyphLocation ensureGlyph(const QString& text, bool bold,
                                        int cellSpan);
    void resetGlyphAtlas();


    // ── 颜色转换 ──────────────────────────────────────────────
    QColor terminalColorToQColor(const NovaTerm::TerminalColor& color,
                                 bool foreground) const;
    QColor highlightColor(NovaTerm::TerminalHighlightRole role) const;

    /**
     * @brief 计算一行的语义高亮角色。
     * @param widgetRow 可见行号。
     * @param screen    本帧快照。
     * @return `TerminalHighlightRole` 的整数值；NovaTerm::NoHighlightRole
     *         表示本行未命中任何规则。
     * @note 行文本写入复用的成员缓冲（旧实现为每个 Cell 构造一个 QString），
     *       并先用字面量前置过滤掉不可能命中的规则再跑 PCRE2。
     */
    int rowHighlightRole(int widgetRow,
                         const NovaTerm::RendererSnapshot& screen);

    // ── Unicode 转 UTF-8 ──────────────────────────────────────
    static QString cellCharsToString(const uint32_t* chars, int maxCount);
    /// 把一个 Cell 的字符簇追加到目标串，不产生临时 QString。
    static void appendCellChars(QString& out, const uint32_t* chars,
                                int maxCount);
    static QShader loadShader(const QString& path);
    void releaseRhiResources();
    void ensureAtlasTexture();
    void ensurePipeline();
    void requestFullFrame();
    void requestOverlayFrame();
    /**
     * @brief 把历史显示行布局增量维护到与当前 scrollback 快照一致。
     * @note  列宽变化或布局尚未建立时改为发起一次异步全量重排；其余情况
     *        只处理头部淘汰与尾条逻辑行的增长，代价 O(新增内容)。
     */
    void updateHistoryLayout();

    /**
     * @brief 执行一次合并后的历史布局维护（scrollbackChanged 的处理器体）。
     *
     * @note  与 `updateHistoryLayout()` 分开是因为后者要走
     *        `TerminalCore::scrollbackTail()`，它把尾部逻辑行深拷贝一份；
     *        一个事件循环轮次里积压的多个解析批次应共用同一次拷贝，故由
     *        scrollbackChanged 置脏并投递一次 queued 调用到本函数。
     */
    void syncHistoryLayout();

    /**
     * @brief 按 _scrollAnchorLine/_scrollAnchorWrap 把滚动偏移还原到同一内容。
     */
    void restoreScrollFromAnchor();

    /**
     * @brief 量程或偏移相对上次发布有变化时发出 scrollStateChanged。
     * @note  所有改动 _scrollLine 或 _historyLayout 的路径末尾都要调用，
     *        比较两个整数即可，重复调用无副作用。
     */
    void publishScrollState();

    /**
     * @brief 选区端点若已失效则清除。
     * @return true 表示确实清除了选区。
     */
    bool dropInvalidSelection();
    void scheduleReflow();
    bool ensureVertexBuffer(int rows, int columns);
    void resetWidgetRowMapping(int rows);
    void updatePlacementBuffer(QRhiResourceUpdateBatch* updates,
                               const QSize& pixelSize);
    void uploadAtlasChanges(QRhiResourceUpdateBatch* updates);
    void uploadCommands(QRhiResourceUpdateBatch* updates,
                        const QSize& pixelSize,
                        const std::vector<bool>& dirtyRows,
                        const QVector<QVector<NovaTerm::DirtyColumnSpan>>& dirtySpans,
                        bool uploadAllRows,
                        bool overlayDirty);
    void recordCpuFrame(quint64 elapsedNanoseconds);

    TerminalCore* _core;
    TerminalColorScheme _scheme;
    QVector<NovaTerm::TerminalHighlightRule> _highlightRules;
    /**
     * @brief 与 _highlightRules 一一对应的字面量前置过滤表。
     *
     * `literals` 为空表示该规则无法前置过滤，必须照旧跑正则。
     * 由 setHighlightRules() 一次性建好，行文本侧每帧只做 O(1) 的
     * `contains` 试探，不再让每条规则对整行跑一次 PCRE2。
     */
    struct HighlightPrefilter
    {
        QVector<QString> literals;
        bool caseInsensitive{false};
    };
    QVector<HighlightPrefilter> _highlightPrefilters;
    /// 行文本复用缓冲（逐 Cell 追加）与它的小写副本（仅大小写不敏感规则用）。
    QString _rowTextScratch;
    QString _rowTextFoldedScratch;
    /// 是否存在需要小写副本的规则，避免为纯大小写敏感规则集做无用功。
    bool _highlightNeedsFoldedText{false};

    QFont _font;
    QFontMetricsF* _fm{nullptr};
    NovaTerm::FontManager _fontManager;
    NovaTerm::AsyncGlyphRasterizer _glyphRasterQueue;
    std::vector<bool> _glyphPendingRows;
    int _buildingGlyphRow{-1};
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
    // 上次经 scrollStateChanged 发布的量程与偏移，用于只在变化时发信号。
    int _publishedMaximumOffset{0};
    int _publishedScrollOffset{0};
    quint64 _reflowGeneration{0};
    /// 有 scrollback 增量待合并（见 syncHistoryLayout 的说明）。
    bool _historyLayoutDirty{false};
    /// 已投递一次合并维护，避免重复排队。
    bool _historyLayoutSyncScheduled{false};
    HistoryLayout _historyLayout;
    QVector<NovaTerm::DisplayLine> _pendingHistoryLayout;
    // _historyLayout 所依据的列宽。0 表示布局未建立。仅当它与当前列宽不一致
    // 时才需要全量重排 —— 行数变化不影响折行。
    int _layoutColumns{0};
    // 正在进行的全量重排所用的列宽，重排完成时提交给 _layoutColumns。
    int _pendingLayoutColumns{0};
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
    bool _autoCopyCurrentSelection{false};
    bool _selectAllPending{false}; // 等待异步历史布局完成后建立全选区。
    // 当前按键手势归属 VT 鼠标（按下时跟踪开启）。release 按此判定转发，
    // 不看 release 时刻的跟踪状态 —— 手势期间应用退出/进入鼠标模式
    // 不能切换半途手势的归属。
    bool _activeGestureIsVtMouse{false};
    NovaTerm::Position _selStart{-1, -1};
    NovaTerm::Position _selEnd{-1, -1};

    // 用于跟踪 wheel 事件累积
    int _wheelAccum{0};
    int _zoomWheelAccum{0};

    QRhi* _rhi{nullptr};
    quint64 _atlasGeneration{0};
    qreal _atlasDpr{0.0};
    quint64 _frameNumber{0};
    // ── 跨帧复用的暂存缓冲（避免每帧堆分配/释放）────────────────
    /**
     * @brief placement uniform 的 CPU 侧副本，固定 kPlacementFloatCount 项。
     *
     * 只写头部 4 项、修正矩阵 16 项与 `[0, rowCount)` 的行槽位；着色器对
     * `instanceMeta.y >= viewport.w` 的槽位提前返回，根本不索引
     * `rowPlacement[]`，因此槽位表的尾部无需每帧清零。
     */
    QVector<float> _placementScratch;
    /// 本帧脏行标记，容量跨帧保留。
    std::vector<bool> _dirtyRowsScratch;
    /// 本帧每行的脏列区间；内层向量容量跨帧保留。
    QVector<QVector<NovaTerm::DirtyColumnSpan>> _dirtySpansScratch;
    /// overlay 命令的复用缓冲：与命令缓冲交换内容，两侧容量都保留。
    QVector<NovaTerm::RenderCommand> _overlayScratch;
    /// uploadAllRows 时复用的"整行宽"列区间，免去每行一个一元素 QVector。
    QVector<NovaTerm::DirtyColumnSpan> _fullRowSpanScratch;
    /**
     * @brief 本帧是否向栅格化队列入队过任务。
     *
     * 用于决定是否需要采样 glyphRasterQueueDepth：该深度来自
     * `AsyncGlyphRasterizer::size()`，会在栅格化互斥锁下线性扫描整张
     * `QHash(GlyphKey,bool>`（上限 MaxPendingTasks = 512）。队列只由
     * enqueue() 增长，所以"深度为 0 且本帧既没入队也没取走结果"必然仍是
     * 0，此时跳过采样；深度非零时继续采样直到归零。
     */
    bool _glyphEnqueuedThisFrame{false};
    QVector<GpuInstance> _instances;
    // 背景/内容分别使用独立的 span scratch：容量跨帧复用（避免每个 span 重新
    // 增长 QList），且两份数据可以分别按实际区间上传，互不干扰。
    QVector<GpuInstance> _backgroundInstances;
    QVector<GpuInstance> _contentInstances;
    // 增量重建时暂存上一帧命令：与目标行向量 swap（只交换指针），使新命令可以
    // 直接写进目标行并复用它已有的容量，同时保留旧命令作为"未脏列"的来源。
    QVector<NovaTerm::RenderCommand> _oldBackgrounds;
    QVector<NovaTerm::RenderCommand> _oldContents;
    NovaTerm::BufferBudget _bufferBudget;
    NovaTerm::RendererCapabilities _capabilities;
    NovaTerm::RowSlotMap _rowSlotMap;
    QVector<quint64> _rowContentIdentities;
    NovaTerm::RowBlockDamageTracker _rowBlockDamageTracker;
    quint64 _viewportMappingRevision{0};
    NovaTerm::RenderCommandBuffer _commandBuffer;
    // QRhi may consume a frame while queued terminal damage/scroll state is
    // being published. Protect the complete hand-off and never combine a
    // live scroll count from one publication window with damage from another.
    QMutex _pendingFrameMutex;
    QVector<NovaTerm::DirtyRegion> _pendingDirtyRegions;
    NovaTerm::ScrollDamageHandoff _scrollDamageHandoff;
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
