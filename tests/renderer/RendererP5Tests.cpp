#include "renderer/font/FontManager.h"
#include "renderer/glyph/GlyphAtlas.h"
#include "renderer/glyph/GlyphCache.h"
#include "renderer/glyph/GlyphRasterizer.h"
#include "renderer/gpu/BufferBudget.h"
#include "renderer/gpu/MaterialBatcher.h"
#include "renderer/gpu/RowSlotMap.h"
#include "renderer/TerminalHighlighting.h"
#include "session/SerialHighlightRules.h"

#include <QTest>

#include <limits>

class RendererP5Tests final : public QObject
{
    Q_OBJECT
private slots:
    void glyphKeyDistinguishesContractFields();
    void fallbackKeepsGridContract();
    void fontAndGlyphGenerationsAreMonotonic();
    void rasterizerPreservesFullClusterAndColorFormat();
    void rasterizedQuadStartsAtCellLocalOrigin();
    void rasterQueueIsBoundedDeduplicatedAndGenerationSafe();
    void rasterQueueStopCancelsPendingWork();
    void atlasSeparatesFormatsAndUploadsDirtyRects();
    void atlasDefersUploadsAtFrameBudget();
    void atlasEvictionHonorsFramesInFlight();
    void atlasResourceRebuildReuploadsResidentPages();
    void glyphCacheRejectsStaleGeneration();
    void glyphCacheWarmHitDoesNotUploadAgain();
    void rowSlotRingReusesScrolledRows();
    void rowSlotMapHandlesJumpAndForcedRemap();
    void mappingRevisionIsIndependent();
    void materialBatchesPreserveLayers();
    void materialBatchesSeparateAtlasPages();
    void bufferOnlyGrowsAndRejectsOverBudget();
    void bufferReleaseRetainsPeakStatistics();
    void semanticHighlightRulesRespectPriorityAndCase();
};

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

void RendererP5Tests::rowSlotRingReusesScrolledRows()
{
    NovaTerm::RowSlotMap map;
    QVector<NovaTerm::VisibleRowIdentity> first = {
        {1, 1, 0, false}, {2, 1, 0, false}, {3, 1, 0, false},
        {4, 1, 0, false}
    };
    auto initial = map.update(first, 20.0f);
    QCOMPARE(initial.enteringWidgetRows.size(), 4);
    QVector<NovaTerm::VisibleRowIdentity> scrolled = {
        first[1], first[2], first[3], {5, 1, 0, false}
    };
    const auto update = map.update(scrolled, 20.0f);
    QCOMPARE(update.reusedRows, 3);
    QCOMPARE(update.enteringWidgetRows, QVector<int>({3}));
    QCOMPARE(map.capacity(), 4);
}

void RendererP5Tests::rowSlotMapHandlesJumpAndForcedRemap()
{
    NovaTerm::RowSlotMap map;
    QVector<NovaTerm::VisibleRowIdentity> rows = {
        {1, 1, 0, false}, {2, 1, 0, false}, {3, 1, 0, false}
    };
    map.update(rows, 18.0f);
    QVector<NovaTerm::VisibleRowIdentity> jump = {
        {20, 1, 0, false}, {21, 1, 0, false}, {22, 1, 0, false}
    };
    const auto jumped = map.update(jump, 18.0f);
    QCOMPARE(jumped.reusedRows, 0);
    QCOMPARE(jumped.enteringWidgetRows.size(), 3);
    QCOMPARE(map.capacity(), 3);
    const auto forced = map.update(jump, 20.0f, true);
    QVERIFY(forced.fullRemap);
    QCOMPARE(forced.reusedRows, 0);
    QCOMPARE(forced.enteringWidgetRows.size(), 3);
    QCOMPARE(map.capacity(), 3);
}

void RendererP5Tests::mappingRevisionIsIndependent()
{
    NovaTerm::RowSlotMap map;
    QVector<NovaTerm::VisibleRowIdentity> rows = {{1, 7, 0, true}};
    const quint64 before = map.mappingRevision();
    map.update(rows, 10);
    QVERIFY(map.mappingRevision() > before);
    const quint64 first = map.mappingRevision();
    map.update(rows, 12);
    QVERIFY(map.mappingRevision() > first);
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

QTEST_MAIN(RendererP5Tests)
#include "RendererP5Tests.moc"
