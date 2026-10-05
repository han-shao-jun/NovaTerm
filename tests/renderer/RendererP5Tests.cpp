#include "core/terminal/TerminalCore.h"
#include "renderer/font/FontManager.h"
#include "renderer/HistoryLayout.h"
#include "renderer/glyph/GlyphAtlas.h"
#include "renderer/glyph/GlyphCache.h"
#include "renderer/glyph/GlyphRasterizer.h"
#include "renderer/gpu/BufferBudget.h"
#include "renderer/gpu/MaterialBatcher.h"
#include "renderer/gpu/RowSlotMap.h"
#include "renderer/RowBlockDamageTracker.h"
#include "renderer/ScrollDamageHandoff.h"
#include "renderer/TerminalHighlighting.h"
#include "session/SerialHighlightRules.h"

#include <QTest>

#include <limits>
#include <atomic>
#include <vector>

class RendererP5Tests final : public QObject
{
    Q_OBJECT
private slots:
    void glyphKeyDistinguishesContractFields();
    void fallbackKeepsGridContract();
    void fontAndGlyphGenerationsAreMonotonic();
    void selectionCacheInvalidatesOnFontChange();
    void rasterizerPreservesFullClusterAndColorFormat();
    void rasterizedQuadStartsAtCellLocalOrigin();
    void rasterQueueIsBoundedDeduplicatedAndGenerationSafe();
    void rasterQueueStopCancelsPendingWork();
    void asyncRasterBurstAndCancellation();
    void historyLogicalHeadPreservesIndices();
    void atlasSeparatesFormatsAndUploadsDirtyRects();
    void atlasDefersUploadsAtFrameBudget();
    void atlasEvictionHonorsFramesInFlight();
    void atlasResourceRebuildReuploadsResidentPages();
    void glyphCacheRejectsStaleGeneration();
    void glyphCacheWarmHitDoesNotUploadAgain();
    void rowSlotRingReusesScrolledRows();
    void sequentialRowSlotsStayValidAcrossResizeAndScroll();
    void rowBlockDamageFindsOmittedStaleTail();
    void scrollDamageHandoffWaitsForContentFrame();
    void materialBatchesPreserveLayers();
    void materialBatchesSeparateAtlasPages();
    void fullRowUploadClearsRetainedStride();
    void bufferOnlyGrowsAndRejectsOverBudget();
    void bufferReleaseRetainsPeakStatistics();
    void semanticHighlightRulesRespectPriorityAndCase();
    void asciiSelectionUsesDirectCacheAndSingleQuery();
};

void RendererP5Tests::historyLogicalHeadPreservesIndices()
{
    HistoryLayout layout;
    for (quint64 id = 1; id <= 100000; ++id) {
        NovaTerm::DisplayLine row;
        row.lineId = id;
        layout.push_back(row);
    }
    for (quint64 id = 100001; id <= 200000; ++id) {
        layout.remove(0, 1);
        NovaTerm::DisplayLine row;
        row.lineId = id;
        layout.push_back(row);
        QCOMPARE(layout.size(), 100000);
        QCOMPARE(layout[0].lineId, id - 99999);
        QCOMPARE(layout.constLast().lineId, id);
    }
    layout.remove(layout.size() - 2, 2);
    QCOMPARE(layout.constLast().lineId, quint64(199998));
    layout.remove(0, layout.size());
    QVERIFY(layout.isEmpty());
}

void RendererP5Tests::asyncRasterBurstAndCancellation()
{
    std::atomic<bool> offThread{false};
    const auto owner = std::this_thread::get_id();
    NovaTerm::AsyncGlyphRasterizer worker;
    worker.setReadyCallback([&] {
        offThread.store(std::this_thread::get_id() != owner);
    });
    QFont font(QStringLiteral("monospace"));
    font.setPixelSize(16);
    for (int i = 0; i < 128; ++i) {
        NovaTerm::GlyphKey key;
        key.cluster = QString(QChar(0x4e00 + i)) + QStringLiteral("\u0301");
        key.fontGeneration = 1;
        key.cellSpan = 2;
        QVERIFY(worker.enqueue({key, font, 10, 20, true}));
        QVERIFY(worker.enqueue({key, font, 10, 20, true}));
    }
    int completed = 0;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        auto results = worker.takeResults();
        for (const auto& bitmap : results) {
            if (bitmap.image.isNull() || bitmap.sourceGeneration != 1)
                return false;
            ++completed;
        }
        return completed == 128;
    })(), 5000);
    QVERIFY(offThread.load());
    worker.cancelBeforeGeneration(2);
    NovaTerm::GlyphKey stale;
    stale.cluster = QStringLiteral("old");
    stale.fontGeneration = 1;
    QVERIFY(!worker.enqueue({stale, font, 10, 20, true}));
    worker.stop();
    QCOMPARE(worker.size(), 0);
    QVERIFY(!worker.enqueue({stale, font, 10, 20, true}));
}

void RendererP5Tests::semanticHighlightRulesRespectPriorityAndCase()
{
    const auto rules = NovaTerm::serialLogHighlightRules();

    QCOMPARE(NovaTerm::matchTerminalHighlight(
                 rules, QStringLiteral("FAILED, retry OK")),
             std::optional(NovaTerm::TerminalHighlightRole::Error));
    QCOMPARE(NovaTerm::matchTerminalHighlight(
                 rules, QStringLiteral("Warning: voltage low")),
             std::optional(NovaTerm::TerminalHighlightRole::Warning));
    QCOMPARE(NovaTerm::matchTerminalHighlight(
                 rules, QStringLiteral("SUCCESSFUL_HANDOFF")),
             std::optional(NovaTerm::TerminalHighlightRole::Success));
    QCOMPARE(NovaTerm::matchTerminalHighlight(
                 rules, QStringLiteral("Zynq> ")),
             std::optional(NovaTerm::TerminalHighlightRole::Prompt));
    QVERIFY(!NovaTerm::matchTerminalHighlight(
                rules, QStringLiteral("ordinary serial output"))
                 .has_value());
}

void RendererP5Tests::glyphKeyDistinguishesContractFields()
{
    NovaTerm::GlyphKey base;
    base.faceId = 1;
    base.fontGeneration = 2;
    base.cluster = QString::fromUtf8("a\xcc\x81");
    base.pixelSize = 16;
    QHash<NovaTerm::GlyphKey, int> keys;
    keys.insert(base, 1);
    auto changed = base;
    changed.cluster = QStringLiteral("á");
    keys.insert(changed, 2);
    changed = base;
    changed.scale1024 = 1280;
    keys.insert(changed, 3);
    changed = base;
    changed.fallbackIndex = 1;
    keys.insert(changed, 4);
    changed = base;
    changed.cellSpan = 2;
    keys.insert(changed, 5);
    changed = base;
    changed.format = NovaTerm::GlyphPixelFormat::Rgba8;
    keys.insert(changed, 6);
    QCOMPARE(keys.size(), 6);
}

void RendererP5Tests::fallbackKeepsGridContract()
{
    QFont primary(QStringLiteral("monospace"));
    primary.setPixelSize(16);
    NovaTerm::FontManager manager(primary);
    manager.setFallbackFamilies({QStringLiteral("Noto Sans CJK SC"),
                                 QStringLiteral("sans-serif")});
    const auto key = manager.makeKey(QStringLiteral("中文"), false, false,
                                     2, 1.25);
    QCOMPARE(key.cluster, QStringLiteral("中文"));
    QCOMPARE(key.cellSpan, 2);
    QCOMPARE(key.scale1024, 1280);
    QVERIFY(key.faceId != 0);
}

void RendererP5Tests::fontAndGlyphGenerationsAreMonotonic()
{
    QFont first(QStringLiteral("monospace"));
    first.setPixelSize(15);
    NovaTerm::FontManager manager(first);
    const quint64 initial = manager.generation();
    manager.setPrimaryFont(first);
    QCOMPARE(manager.generation(), initial);
    QFont second(first);
    second.setPixelSize(17);
    manager.setPrimaryFont(second);
    QVERIFY(manager.generation() > initial);
    const auto key = manager.makeKey(QStringLiteral("x"), true, true,
                                     1, 2.0);
    QCOMPARE(key.fontGeneration, manager.generation());
    QCOMPARE(key.scale1024, 2048);
    QVERIFY(key.italic);
    QVERIFY(key.weight >= int(QFont::DemiBold));
}

// select()/makeKey() 现在缓存 coverage 结果（避免每 Cell 探测字体，P5 §5.2）。
// 缓存必须随字体/fallback 变化失效，否则会返回过期选择。此测试先暖一次缓存，
// 再改主字体与 fallback，验证 faceId/generation 都随之更新，不吃旧缓存。
void RendererP5Tests::selectionCacheInvalidatesOnFontChange()
{
    QFont primary(QStringLiteral("monospace"));
    primary.setPixelSize(16);
    NovaTerm::FontManager manager(primary);

    // 暖缓存：同一簇多次选择结果应一致（命中缓存）。
    const auto first = manager.select(QStringLiteral("A"));
    const auto firstAgain = manager.select(QStringLiteral("A"));
    QCOMPARE(first.faceId, firstAgain.faceId);
    const quint64 gen0 = manager.generation();
    const auto key0 = manager.makeKey(QStringLiteral("A"), false, false, 1, 1.0);
    QCOMPARE(key0.fontGeneration, gen0);

    // 改字号：generation 递增，缓存必须失效，makeKey 反映新 generation 与
    // 新 faceId（idFor 含 pixelSize，故必变）。
    QFont bigger(primary);
    bigger.setPixelSize(24);
    manager.setPrimaryFont(bigger);
    QVERIFY(manager.generation() > gen0);
    const auto afterFont = manager.select(QStringLiteral("A"));
    QVERIFY(afterFont.faceId != first.faceId);
    const auto key1 = manager.makeKey(QStringLiteral("A"), false, false, 1, 1.0);
    QCOMPARE(key1.fontGeneration, manager.generation());

    // 改 fallback：generation 再增，缓存再次失效（此前 select 已重新暖过缓存）。
    const quint64 gen1 = manager.generation();
    manager.setFallbackFamilies({QStringLiteral("Noto Sans CJK SC")});
    QVERIFY(manager.generation() > gen1);
    const auto key2 = manager.makeKey(QStringLiteral("A"), false, false, 1, 1.0);
    QCOMPARE(key2.fontGeneration, manager.generation());
}

void RendererP5Tests::rasterizerPreservesFullClusterAndColorFormat()
{
    QFont font(QStringLiteral("sans-serif"));
    font.setPixelSize(18);
    NovaTerm::GlyphKey key;
    key.cluster = QString::fromUtf8("👩‍💻");
    key.fontGeneration = 7;
    key.cellSpan = 2;
    key.scale1024 = 1024;
    key.renderMode = NovaTerm::GlyphRenderMode::Color;
    key.format = NovaTerm::GlyphPixelFormat::Rgba8;
    const NovaTerm::GlyphBitmap result =
        NovaTerm::GlyphRasterizer().rasterize(key, font, 10, 20);
    QCOMPARE(result.key.cluster, key.cluster);
    QCOMPARE(result.key.format, NovaTerm::GlyphPixelFormat::Rgba8);
    QCOMPARE(result.sourceGeneration, quint64(7));
    QCOMPARE(result.cellSpan, 2);
    QVERIFY(!result.image.isNull());
    QVERIFY(result.logicalRect.width() >= 20.0);
}

void RendererP5Tests::rasterizedQuadStartsAtCellLocalOrigin()
{
    QFont font(QStringLiteral("monospace"));
    font.setPixelSize(16);
    NovaTerm::GlyphKey key;
    key.cluster = QStringLiteral("M");
    key.fontGeneration = 1;
    key.cellSpan = 1;
    key.scale1024 = 1280;
    const qreal cellWidth = 10.0;
    const qreal cellHeight = 20.0;
    const NovaTerm::GlyphBitmap result =
        NovaTerm::GlyphRasterizer().rasterize(key, font,
                                              cellWidth, cellHeight);

    QCOMPARE(result.logicalRect.topLeft(), QPointF(0, 0));
    QVERIFY(result.logicalRect.width() >= cellWidth);
    QVERIFY(result.logicalRect.height() >= cellHeight);
    QCOMPARE(qCeil(result.logicalRect.width() * 1.25),
             result.image.width());
    QCOMPARE(qCeil(result.logicalRect.height() * 1.25),
             result.image.height());
    QVERIFY(result.baseline > 0);
}

void RendererP5Tests::rasterQueueIsBoundedDeduplicatedAndGenerationSafe()
{
    NovaTerm::BoundedGlyphRasterQueue queue(2);
    NovaTerm::BoundedGlyphRasterQueue::Task a;
    a.key.cluster = QStringLiteral("a");
    a.key.fontGeneration = 1;
    a.visible = false;
    auto b = a;
    b.key.cluster = QStringLiteral("b");
    b.key.fontGeneration = 2;
    b.visible = true;
    auto c = b;
    c.key.cluster = QStringLiteral("c");
    QVERIFY(queue.enqueue(a));
    QVERIFY(queue.enqueue(a));
    QVERIFY(queue.enqueue(b));
    QVERIFY(!queue.enqueue(c));
    QCOMPARE(queue.statistics().deduplicated, quint64(1));
    QCOMPARE(queue.take()->key.cluster, QStringLiteral("b"));
    queue.cancelBeforeGeneration(2);
    QVERIFY(!queue.take().has_value());
    QCOMPARE(queue.statistics().staleDropped, quint64(1));
}

void RendererP5Tests::rasterQueueStopCancelsPendingWork()
{
    NovaTerm::BoundedGlyphRasterQueue queue(4);
    NovaTerm::BoundedGlyphRasterQueue::Task task;
    task.key.cluster = QStringLiteral("pending");
    QVERIFY(queue.enqueue(task));
    queue.stop();
    QVERIFY(queue.stopped());
    QCOMPARE(queue.size(), qsizetype(0));
    QCOMPARE(queue.statistics().cancelled, quint64(1));
    QVERIFY(!queue.enqueue(task));
}

static NovaTerm::GlyphBitmap bitmap(const QString& cluster,
                                    NovaTerm::GlyphPixelFormat format,
                                    QSize size = QSize(8, 8))
{
    NovaTerm::GlyphBitmap result;
    result.key.cluster = cluster;
    result.key.fontGeneration = 1;
    result.key.format = format;
    result.sourceGeneration = 1;
    result.image = QImage(size, QImage::Format_RGBA8888);
    result.image.fill(Qt::white);
    result.logicalRect = QRectF(QPointF(), size);
    return result;
}

void RendererP5Tests::atlasSeparatesFormatsAndUploadsDirtyRects()
{
    NovaTerm::GlyphAtlasConfig config;
    config.pageSize = QSize(64, 64);
    config.byteBudget = 2 * 64 * 64 * 4;
    NovaTerm::GlyphAtlas atlas(config);
    const auto gray = atlas.insert(bitmap(QStringLiteral("a"),
                                          NovaTerm::GlyphPixelFormat::Alpha8), 1);
    const auto color = atlas.insert(bitmap(QStringLiteral("😀"),
                                           NovaTerm::GlyphPixelFormat::Rgba8), 1);
    QVERIFY(gray && color);
    QVERIFY(gray->pageId != color->pageId);
    auto uploads = atlas.takeUploads();
    QCOMPARE(uploads.size(), 2); // first upload of each newly created page
    atlas.insert(bitmap(QStringLiteral("b"),
                        NovaTerm::GlyphPixelFormat::Alpha8), 10);
    uploads = atlas.takeUploads();
    QCOMPARE(uploads.size(), 1);
    QVERIFY(!uploads.front().fullPage);
    QVERIFY(uploads.front().bytes() < quint64(64 * 64 * 4));
}

void RendererP5Tests::atlasDefersUploadsAtFrameBudget()
{
    NovaTerm::GlyphAtlasConfig config;
    config.pageSize = QSize(64, 64);
    config.byteBudget = 2 * 64 * 64 * 4;
    config.uploadBudgetPerFrame = 256;
    NovaTerm::GlyphAtlas atlas(config);
    QVERIFY(atlas.insert(bitmap(QStringLiteral("gray-cold"),
                                NovaTerm::GlyphPixelFormat::Alpha8), 1));
    QVERIFY(atlas.insert(bitmap(QStringLiteral("color-cold"),
                                NovaTerm::GlyphPixelFormat::Rgba8), 1));
    // Drain cold full-page uploads independently of the steady-state check.
    while (!atlas.takeUploads().isEmpty()) {}

    QVERIFY(atlas.insert(bitmap(QStringLiteral("gray-warm"),
                                NovaTerm::GlyphPixelFormat::Alpha8), 10));
    QVERIFY(atlas.insert(bitmap(QStringLiteral("color-warm"),
                                NovaTerm::GlyphPixelFormat::Rgba8), 10));
    const auto firstFrame = atlas.takeUploads(256);
    QCOMPARE(firstFrame.size(), 1);
    QVERIFY(atlas.hasPendingUploads());
    QVERIFY(atlas.statistics().deferredUploadBytes >= quint64(256));
    const auto secondFrame = atlas.takeUploads(256);
    QCOMPARE(secondFrame.size(), 1);
    QVERIFY(atlas.takeUploads(256).isEmpty());
    QVERIFY(!atlas.hasPendingUploads());
}

void RendererP5Tests::atlasEvictionHonorsFramesInFlight()
{
    NovaTerm::GlyphAtlasConfig config;
    config.pageSize = QSize(16, 16);
    config.byteBudget = 16 * 16 * 4;
    config.padding = 1;
    config.framesInFlight = 3;
    NovaTerm::GlyphAtlas atlas(config);
    const auto first = atlas.insert(bitmap(QStringLiteral("a"),
                                           NovaTerm::GlyphPixelFormat::Alpha8,
                                           QSize(12, 12)), 1);
    QVERIFY(first);
    QVERIFY(!atlas.insert(bitmap(QStringLiteral("b"),
                                 NovaTerm::GlyphPixelFormat::Alpha8,
                                 QSize(12, 12)), 2));
    const auto second = atlas.insert(bitmap(QStringLiteral("b"),
                                            NovaTerm::GlyphPixelFormat::Alpha8,
                                            QSize(12, 12)), 5);
    QVERIFY(second);
    QVERIFY(!atlas.isValid(*first));
    QVERIFY(atlas.isValid(*second));
}

void RendererP5Tests::atlasResourceRebuildReuploadsResidentPages()
{
    NovaTerm::GlyphAtlasConfig config;
    config.pageSize = QSize(32, 32);
    const quint64 pageBytes = 32 * 32 * 4;
    config.byteBudget = 2 * pageBytes;
    NovaTerm::GlyphAtlas atlas(config);
    QVERIFY(atlas.insert(bitmap(QStringLiteral("r"),
                                NovaTerm::GlyphPixelFormat::Alpha8), 1));
    QVERIFY(atlas.insert(bitmap(QStringLiteral("😀"),
                                NovaTerm::GlyphPixelFormat::Rgba8), 1));
    atlas.takeUploads();
    QVERIFY(atlas.takeUploads().isEmpty());
    atlas.markAllDirty();
    QVERIFY(atlas.hasFullPageUploads());
    const auto restored = atlas.takeUploads(
        std::numeric_limits<quint64>::max());
    QCOMPARE(restored.size(), 2);
    QVERIFY(restored[0].fullPage);
    QVERIFY(restored[1].fullPage);
    QCOMPARE(restored[0].bytes(), pageBytes);
    QCOMPARE(restored[1].bytes(), pageBytes);
    QVERIFY(!atlas.hasPendingUploads());
    QVERIFY(!atlas.hasFullPageUploads());
}

void RendererP5Tests::glyphCacheRejectsStaleGeneration()
{
    NovaTerm::GlyphAtlasConfig config;
    config.pageSize = QSize(32, 32);
    config.byteBudget = 32 * 32 * 4;
    NovaTerm::GlyphCache cache(config);
    auto stale = bitmap(QStringLiteral("x"), NovaTerm::GlyphPixelFormat::Alpha8);
    stale.sourceGeneration = 2;
    QVERIFY(!cache.insert(stale, 1));
    QCOMPARE(cache.statistics().failed, quint64(1));
}

void RendererP5Tests::glyphCacheWarmHitDoesNotUploadAgain()
{
    NovaTerm::GlyphAtlasConfig config;
    config.pageSize = QSize(32, 32);
    config.byteBudget = 32 * 32 * 4;
    NovaTerm::GlyphCache cache(config);
    const auto glyph = bitmap(QStringLiteral("warm"),
                              NovaTerm::GlyphPixelFormat::Alpha8);
    const auto inserted = cache.insert(glyph, 1);
    QVERIFY(inserted);
    cache.atlas().takeUploads();
    const auto found = cache.find(glyph.key, 2);
    QVERIFY(found);
    QCOMPARE(found->pageId, inserted->pageId);
    QCOMPARE(found->pageGeneration, inserted->pageGeneration);
    QCOMPARE(found->pixelRect, inserted->pixelRect);
    QVERIFY(cache.atlas().takeUploads().isEmpty());
    QVERIFY(cache.statistics().hits >= quint64(1));
}

// 固定槽位环的滚动语义：上滚 count 行后，原第 count..rows-1 行保留自己的
// 槽位（GPU 侧顶点不用重传），只有新进入底部的行拿到原来顶部的槽位。
//
// 行身份哈希式的增量槽位分配（RowSlotMap::update）与它的
// rowsNeedingRebuildAfterMapping 已随渲染器的 live-scroll 旋转快路径删除：
// 那条快路径在生产配置下不可达，判定所需的 shell 能力位也还没有。见
// src/renderer/gpu/RowSlotMap.h 文件头。
void RendererP5Tests::rowSlotRingReusesScrolledRows()
{
    NovaTerm::RowSlotMap map;
    map.resetSequential(4, 20.0f);
    QVERIFY(map.isValidPermutation(4));
    QCOMPARE(map.slotForWidgetRow(0), 0);
    QCOMPARE(map.slotForWidgetRow(3), 3);

    map.rotateRowsUp(1, 20.0f);
    QVERIFY(map.isValidPermutation(4));
    // 新顶行 = 原第 1 行，槽位仍是 1。
    QCOMPARE(map.slotForWidgetRow(0), 1);
    QCOMPARE(map.slotForWidgetRow(2), 3);
    // 新底行拿到原顶行的槽位 0（该槽位的内容已过时，必须重传）。
    QCOMPARE(map.slotForWidgetRow(3), 0);
    // widgetRow 与 yTransform 必须随旋转重算，否则着色器把行画到错的高度。
    for (int widgetRow = 0; widgetRow < 4; ++widgetRow) {
        QCOMPARE(map.placements()[widgetRow].widgetRow, widgetRow);
        QCOMPARE(map.placements()[widgetRow].yTransform,
                 float(widgetRow * 20));
    }

    // 超出范围的滚动量被夹到行数，且不破坏排列。
    map.rotateRowsUp(99, 20.0f);
    QVERIFY(map.isValidPermutation(4));
    QCOMPARE(map.slotForWidgetRow(0), 1);

    // 行数收缩后旧槽位越界，必须被 isValidPermutation 判为非法排列，
    // 由渲染器 resetWidgetRowMapping 兜底。
    map.rotateRowsUp(0, 20.0f);
    QVERIFY(!map.isValidPermutation(3));
    QVERIFY(!map.isValidPermutation(5));
}

void RendererP5Tests::fullRowUploadClearsRetainedStride()
{
    // The GPU row stride intentionally retains its fullscreen capacity after
    // a shrink. Incremental updates touch only active cells, while a full
    // resize upload must overwrite the retained tail so stale instances are
    // not interpreted through the new row/content offsets.
    QCOMPARE(NovaTerm::rowUploadVertexCount(80, 160, false), 80);
    QCOMPARE(NovaTerm::rowUploadVertexCount(80, 160, true), 160);
    QCOMPARE(NovaTerm::rowUploadVertexCount(320, 640, true), 640);
    QCOMPARE(NovaTerm::rowUploadVertexCount(320, 160, true), 320);
}

void RendererP5Tests::sequentialRowSlotsStayValidAcrossResizeAndScroll()
{
    NovaTerm::RowSlotMap map;
    map.resetSequential(5, 18.0f);
    QVERIFY(map.isValidPermutation(5));

    map.rotateRowsUp(2, 18.0f);
    QVERIFY(map.isValidPermutation(5));
    QCOMPARE(map.slotForWidgetRow(0), 2);
    QCOMPARE(map.slotForWidgetRow(1), 3);
    QCOMPARE(map.slotForWidgetRow(2), 4);
    QCOMPARE(map.slotForWidgetRow(3), 0);
    QCOMPARE(map.slotForWidgetRow(4), 1);

    map.resetSequential(3, 20.0f);
    QVERIFY(map.isValidPermutation(3));
    QCOMPARE(map.slotForWidgetRow(0), 0);
    QCOMPARE(map.slotForWidgetRow(1), 1);
    QCOMPARE(map.slotForWidgetRow(2), 2);
    QCOMPARE(map.slotForWidgetRow(3), -1);
}

void RendererP5Tests::rowBlockDamageFindsOmittedStaleTail()
{
    NovaTerm::RowBlockDamageTracker tracker;
    tracker.reset(1, 16);
    TerminalCore core(16, 1);
    core.writeInput(QByteArrayLiteral("\x1b[1;9Hxxxxxxxx"));
    QVERIFY(core.waitForIdle());
    const std::vector<bool> dirty(1, true);
    auto snapshot = core.rendererSnapshot(dirty, 0);

    auto spans = tracker.reconcileRow(
        0, snapshot.visibleRowBlockIdentities[0], 16, {});
    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans[0].startColumn, 0);
    QCOMPARE(spans[0].endColumn, 16);

    // ConPTY only reports the rewritten prefix, while the final snapshot has
    // also cleared the old suffix. The tracker must append that omitted block.
    core.writeInput(QByteArrayLiteral("\x1b[1;9H\x1b[8X"));
    QVERIFY(core.waitForIdle());
    snapshot = core.rendererSnapshot(dirty, 0);
    spans = tracker.reconcileRow(
        0, snapshot.visibleRowBlockIdentities[0], 16, {{0, 8}});
    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans[0].startColumn, 0);
    QCOMPARE(spans[0].endColumn, 16);

    // Once synchronized, an ordinary prefix update remains block-local.
    core.writeInput(QByteArrayLiteral("\x1b[1;1Hy"));
    QVERIFY(core.waitForIdle());
    snapshot = core.rendererSnapshot(dirty, 0);
    spans = tracker.reconcileRow(
        0, snapshot.visibleRowBlockIdentities[0], 16, {{0, 8}});
    QCOMPARE(spans.size(), 1);
    QCOMPARE(spans[0].startColumn, 0);
    QCOMPARE(spans[0].endColumn, 8);
}

void RendererP5Tests::scrollDamageHandoffWaitsForContentFrame()
{
    // 只用 takePending() 一个观察点（queuedRows()/pendingRows() 无生产调用方，
    // 已作为死代码删除）：内容帧到达前必须取不到任何行数。
    NovaTerm::ScrollDamageHandoff handoff;
    handoff.queue(3);

    QCOMPARE(handoff.takePending(), 0);
    QCOMPARE(handoff.takePending(), 0);

    handoff.publish();
    QCOMPARE(handoff.takePending(), 3);
    QCOMPARE(handoff.takePending(), 0);

    // A later publication must survive consumption of the current frame.
    handoff.queue(2);
    handoff.publish();
    handoff.queue(1);
    QCOMPARE(handoff.takePending(), 2);
    // 本帧只消费了已 publish 的 2 行；新到但未 publish 的 1 行留给下一帧。
    QCOMPARE(handoff.takePending(), 0);
    handoff.publish();
    QCOMPARE(handoff.takePending(), 1);

    // 饱和而非回绕：溢出后夹到 INT_MAX，让调用方走整屏重建而不是旋转
    // 一个负数行数。
    handoff.queue(std::numeric_limits<int>::max());
    handoff.queue(1);
    handoff.publish();
    QCOMPARE(handoff.takePending(), std::numeric_limits<int>::max());
}

void RendererP5Tests::materialBatchesPreserveLayers()
{
    NovaTerm::RenderCommand background;
    background.type = NovaTerm::RenderCommandType::BackgroundRect;
    NovaTerm::RenderCommand glyph;
    glyph.type = NovaTerm::RenderCommandType::GlyphInstance;
    glyph.atlasPage = 1;
    NovaTerm::RenderCommand overlay;
    overlay.type = NovaTerm::RenderCommandType::Cursor;
    const auto batches = NovaTerm::MaterialBatcher::build(
        {background}, {glyph}, {overlay});
    QCOMPARE(batches.size(), 3);
    QCOMPARE(batches[0].key.layer, NovaTerm::RenderLayer::Background);
    QCOMPARE(batches[1].key.layer, NovaTerm::RenderLayer::Content);
    QCOMPARE(batches[2].key.layer, NovaTerm::RenderLayer::Overlay);
}

void RendererP5Tests::materialBatchesSeparateAtlasPages()
{
    NovaTerm::RenderCommand page0;
    page0.type = NovaTerm::RenderCommandType::GlyphInstance;
    page0.atlasPage = 0;
    auto page1 = page0;
    page1.atlasPage = 1;
    const auto batches = NovaTerm::MaterialBatcher::build({},
                                                          {page0, page1, page0},
                                                          {});
    QCOMPARE(batches.size(), 2);
    QCOMPARE(batches[0].key.layer, NovaTerm::RenderLayer::Content);
    QCOMPARE(batches[0].commands.size(), 2);
    QCOMPARE(batches[1].commands.size(), 1);
    QVERIFY(batches[0].key.atlasPage != batches[1].key.atlasPage);
}

void RendererP5Tests::bufferOnlyGrowsAndRejectsOverBudget()
{
    NovaTerm::BufferBudget budget(1024, 128);
    QCOMPARE(*budget.capacityFor(100), quint64(128));
    const quint64 first = budget.capacity();
    QCOMPARE(*budget.capacityFor(64), first);
    QCOMPARE(budget.statistics().reallocations, quint64(1));
    QVERIFY(budget.capacityFor(900));
    QVERIFY(!budget.capacityFor(2048));
    QCOMPARE(budget.statistics().budgetRejections, quint64(1));
}

void RendererP5Tests::bufferReleaseRetainsPeakStatistics()
{
    NovaTerm::BufferBudget budget(4096, 256);
    QVERIFY(budget.capacityFor(1024));
    const quint64 peak = budget.statistics().peakBytes;
    budget.release();
    QCOMPARE(budget.capacity(), quint64(0));
    QCOMPARE(budget.statistics().currentBytes, quint64(0));
    QCOMPARE(budget.statistics().peakBytes, peak);
    QVERIFY(budget.capacityFor(512));
    QCOMPARE(budget.statistics().reallocations, quint64(2));
}

// ASCII 稳态路径：单码点簇走直连缓存，一次 makeKeyAndSelection 只查询一次字体
// 选择；第二次同簇不再做 coverage 探测，font generation 变化后整体失效。
// 该断言同时守住"ensureGlyph 不再重复 select"的契约：窄接口一次调用即返回
// GlyphKey + FontSelection，miss 路径直接复用，渲染层不再有第二次选择。
void RendererP5Tests::asciiSelectionUsesDirectCacheAndSingleQuery()
{
    QFont primary(QStringLiteral("monospace"));
    primary.setPixelSize(16);
    NovaTerm::FontManager manager(primary);

    const quint64 queriesBefore = manager.selectionQueryCount();
    const quint64 probesBefore = manager.selectionProbeCount();

    const auto first = manager.makeKeyAndSelection(QStringLiteral("A"), false,
                                                   false, 1, 1.0);
    QCOMPARE(manager.selectionQueryCount() - queriesBefore, quint64(1));
    QCOMPARE(manager.selectionProbeCount() - probesBefore, quint64(1));
    QCOMPARE(first.key.cluster, QStringLiteral("A"));
    QVERIFY(first.key.faceId != 0);

    // 第二次：ASCII 直连缓存命中，只增加查询计数、不再探测候选字体。
    const auto second = manager.makeKeyAndSelection(QStringLiteral("A"), false,
                                                    false, 1, 1.0);
    QCOMPARE(manager.selectionQueryCount() - queriesBefore, quint64(2));
    QCOMPARE(manager.selectionProbeCount() - probesBefore, quint64(1));
    QCOMPARE(second.key.faceId, first.key.faceId);

    // 样式位属于缓存键：粗体是独立条目，需要各自探测一次。
    (void)manager.makeKeyAndSelection(QStringLiteral("A"), true, false, 1, 1.0);
    QCOMPARE(manager.selectionProbeCount() - probesBefore, quint64(2));

    // 非 ASCII 簇仍走 QHash 缓存：第二次查询不再探测。
    (void)manager.makeKeyAndSelection(QString::fromUtf8("中"), false, false, 2,
                                      1.0);
    const quint64 probesAfterWide =
        manager.selectionProbeCount() - probesBefore;
    (void)manager.makeKeyAndSelection(QString::fromUtf8("中"), false, false, 2,
                                      1.0);
    QCOMPARE(manager.selectionProbeCount() - probesBefore, probesAfterWide);

    // font generation 变化后 ASCII 直连缓存整体失效，必须重新探测。
    QFont other(primary);
    other.setPixelSize(19);
    manager.setPrimaryFont(other);
    (void)manager.makeKeyAndSelection(QStringLiteral("A"), false, false, 1, 1.0);
    QVERIFY(manager.selectionProbeCount() - probesBefore > probesAfterWide);
}

QTEST_MAIN(RendererP5Tests)
#include "RendererP5Tests.moc"