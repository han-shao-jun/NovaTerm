#include "renderer/RenderCommandBuffer.h"
#include "renderer/RenderScheduler.h"
#include "renderer/TerminalRenderer.h"
#include "core/terminal/TerminalCore.h"

#include <QSignalSpy>
#include <QInputMethodEvent>
#include <QTest>

class RendererP3Tests : public QObject
{
    Q_OBJECT

private slots:
    void schedulerMergesTouchingRegions();
    void schedulerDoesNotMergeCornerOnlyRegions();
    void schedulerClipsAndIgnoresEmptyRegions();
    void schedulerPromotesLargeDamage();
    void schedulerPromotesAtExactCoverageThreshold();
    void schedulerPromotesTooManyRegions();
    void schedulerCoalescesFrameRequests();
    void schedulerSubmitsOverlayOnlyFrame();
    void schedulerPublishesNewestContentRevision();
    void schedulerCancelDropsContentRevision();
    void schedulerFullFrameDominatesLaterDamage();
    void schedulerRejectsUnsupportedRefreshRate();
    void commandBufferReplacesOnlyDirtyRow();
    void commandBufferResizeInvalidatesRows();
    void commandRowsTrackAtlasGeneration();
    void commandBufferValidatesAtlasGeneration();
    void scrollbackAtLiveBottomDoesNotRequestFullFrame();
    void liveBottomMaintainsHistoryLayout();
    void batchedScreenScrollPublishesExactRowCount();
    void enteringHistoryReusesHistoryLayout();
    void returningToLiveBottomKeepsHistoryLayout();
    void columnChangeRequestsReflowRowChangeDoesNot();
    void softWrappedSelectionCopiesAsSingleLine();
    void hardBreakSelectionKeepsNewline();
    void searchMatchesAppendByGeneration();
    void inputMethodCommitProducesUtf8();
    void fragmentedOutputDoesNotInflateScrollbackBytes();
    void spanAssemblyCoversBackgroundSlots();
    void spanAssemblyPacksContentIntoFourSlotsPerCell();
    void spanAssemblyOffsetsMatchRequestedSpan();
    void incrementalRowMergeKeepsOrderAndColumns();
    void mutableRowRebuildKeepsMetadataAndDropsOutOfRange();
};

namespace {

// 构造一条属于指定列的命令；rect.left 用列号编码，便于断言落位。
NovaTerm::RenderCommand commandAtColumn(int column, int atlasPage,
                                        bool colorGlyph)
{
    NovaTerm::RenderCommand command;
    command.type = NovaTerm::RenderCommandType::GlyphInstance;
    command.rect = QRectF(column * 10.0, 4.0, 10.0, 18.0);
    command.uvRect = QRectF(0.25, 0.5, 0.125, 0.25);
    command.color = QColor(11, 22, 33, 255);
    command.atlasPage = atlasPage;
    command.pageGeneration = 9;
    command.cellColumn = column;
    command.colorGlyph = colorGlyph;
    return command;
}

bool isDegenerate(const TerminalRenderer::GpuInstance& instance)
{
    return instance.left == 0.0f && instance.top == 0.0f
        && instance.right == 0.0f && instance.bottom == 0.0f;
}

bool sameInstance(const TerminalRenderer::GpuInstance& a,
                  const TerminalRenderer::GpuInstance& b)
{
    return isDegenerate(a) == isDegenerate(b) && a.left == b.left
        && a.top == b.top && a.right == b.right && a.bottom == b.bottom
        && a.u0 == b.u0 && a.v0 == b.v0 && a.u1 == b.u1 && a.v1 == b.v1
        && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a
        && a.atlasPage == b.atlasPage && a.rowSlot == b.rowSlot
        && a.flags == b.flags && a.reserved == b.reserved;
}

} // namespace

void RendererP3Tests::spanAssemblyCoversBackgroundSlots()
{
    NovaTerm::RenderCommandRow row;
    row.backgrounds.push_back(commandAtColumn(3, 4, false));
    row.backgrounds.push_back(commandAtColumn(5, 4, false));

    QVector<TerminalRenderer::GpuInstance> background;
    QVector<TerminalRenderer::GpuInstance> content;
    const auto counts = TerminalRenderer::assembleSpanInstances(
        row, 2, 6, 7, background, content);

    // 背景每列一个槽位；内容每列固定 4 个槽位（即使没有命令）。
    QCOMPARE(counts.backgroundCount, 4);
    QCOMPARE(counts.contentCount, 16);
    QCOMPARE(background.size(), 4);
    QCOMPARE(content.size(), 16);

    // 没有背景命令的列（2、4）写退化实例，不残留上一帧内容。
    QVERIFY(isDegenerate(background[0]));
    QVERIFY(isDegenerate(background[2]));
    // 有命令的列按局部偏移落位，并带上 atlas 页/行槽/标志位。
    QVERIFY(!isDegenerate(background[1]));
    QCOMPARE(background[1].left, float(3 * 10.0));
    QCOMPARE(background[1].atlasPage, 4.0f);
    QCOMPARE(background[1].rowSlot, 7.0f);
    QCOMPARE(background[1].flags, 0.0f);
    QCOMPARE(background[3].left, float(5 * 10.0));

    // 内容全零：没有内容命令时不能留下任何有效四边形。
    for (const auto& instance : content)
        QVERIFY(isDegenerate(instance));
}

void RendererP3Tests::spanAssemblyPacksContentIntoFourSlotsPerCell()
{
    NovaTerm::RenderCommandRow row;
    // 列 3：1 条；列 4：2 条；列 5：5 条（超出 4 槽位的第 5 条必须丢弃）。
    row.contents.push_back(commandAtColumn(3, 1, false));
    for (int i = 0; i < 2; ++i)
        row.contents.push_back(commandAtColumn(4, 2 + i, false));
    for (int i = 0; i < 5; ++i)
        row.contents.push_back(commandAtColumn(5, 10 + i, true));

    QVector<TerminalRenderer::GpuInstance> background;
    QVector<TerminalRenderer::GpuInstance> content;
    const auto counts = TerminalRenderer::assembleSpanInstances(
        row, 3, 7, 0, background, content);
    QCOMPARE(counts.contentCount, 16);
    QCOMPARE(content.size(), 16);

    // 列 3 是局部第 0 格：仅槽位 0 有效。
    QVERIFY(!isDegenerate(content[0]));
    QCOMPARE(content[0].atlasPage, 1.0f);
    for (int slot = 1; slot < 4; ++slot)
        QVERIFY(isDegenerate(content[slot]));

    // 列 4 是局部第 1 格：槽位 0、1 有效，页号按命令顺序递增。
    QVERIFY(!isDegenerate(content[4]));
    QVERIFY(!isDegenerate(content[5]));
    QCOMPARE(content[4].atlasPage, 2.0f);
    QCOMPARE(content[5].atlasPage, 3.0f);
    QVERIFY(isDegenerate(content[6]));
    QVERIFY(isDegenerate(content[7]));

    // 列 5 是局部第 2 格：4 个槽位全部有效，第 5 条被丢弃。
    for (int slot = 0; slot < 4; ++slot) {
        QVERIFY(!isDegenerate(content[8 + slot]));
        QCOMPARE(content[8 + slot].atlasPage, float(10 + slot));
        QCOMPARE(content[8 + slot].flags, 1.0f);
    }
    // 列 6 没有命令：整格退化。
    for (int slot = 0; slot < 4; ++slot)
        QVERIFY(isDegenerate(content[12 + slot]));
}

void RendererP3Tests::spanAssemblyOffsetsMatchRequestedSpan()
{
    NovaTerm::RenderCommandRow row;
    for (int column = 2; column < 8; ++column) {
        row.backgrounds.push_back(commandAtColumn(column, 1, false));
        row.contents.push_back(commandAtColumn(column, 1, false));
    }

    QVector<TerminalRenderer::GpuInstance> wideBackground;
    QVector<TerminalRenderer::GpuInstance> wideContent;
    const auto wideCounts = TerminalRenderer::assembleSpanInstances(
        row, 0, 8, 3, wideBackground, wideContent);
    QCOMPARE(wideCounts.backgroundCount, 8);
    QCOMPARE(wideCounts.contentCount, 32);

    // 同一行的局部 span：起始列 4 时，scratch[0] 必须对应第 4 列，
    // 且与整行装配的同列实例逐字段一致（offset 映射不能错位）。
    QVector<TerminalRenderer::GpuInstance> narrowBackground;
    QVector<TerminalRenderer::GpuInstance> narrowContent;
    const auto counts = TerminalRenderer::assembleSpanInstances(
        row, 4, 8, 3, narrowBackground, narrowContent);
    QCOMPARE(counts.backgroundCount, 4);
    QCOMPARE(narrowBackground.size(), 4);
    QCOMPARE(narrowContent.size(), 16);
    for (int index = 0; index < 4; ++index)
        QVERIFY(sameInstance(narrowBackground[index], wideBackground[4 + index]));
    for (int index = 0; index < 16; ++index)
        QVERIFY(sameInstance(narrowContent[index], wideContent[16 + index]));

    // 空区间：不产出任何实例，且 scratch 被清空而不是保留旧内容。
    const auto empty = TerminalRenderer::assembleSpanInstances(
        row, 5, 5, 3, narrowBackground, narrowContent);
    QCOMPARE(empty.backgroundCount, 0);
    QCOMPARE(empty.contentCount, 0);
    QVERIFY(narrowBackground.isEmpty());
    QVERIFY(narrowContent.isEmpty());
}

void RendererP3Tests::incrementalRowMergeKeepsOrderAndColumns()
{
    // 上一帧：列 0..5 各一条背景；列 0..5 各一条内容，列 4 额外一条下划线；
    // 另有一条 cellColumn = -1 的行级命令（必须原样保留在最前）。
    QVector<NovaTerm::RenderCommand> oldBackgrounds;
    QVector<NovaTerm::RenderCommand> oldContents;
    oldBackgrounds.push_back(commandAtColumn(-1, 99, false));
    for (int column = 0; column < 6; ++column)
        oldBackgrounds.push_back(commandAtColumn(column, 1, false));
    for (int column = 0; column < 6; ++column) {
        oldContents.push_back(commandAtColumn(column, 2, false));
        if (column == 4)
            oldContents.push_back(commandAtColumn(column, 3, false));
    }

    // 本次只有 [2,4) 脏：列 2、3 各重新生成 1 条背景 + 2 条内容。
    const QVector<NovaTerm::DirtyColumnSpan> spans{{2, 4}};
    QVector<NovaTerm::RenderCommand> backgrounds;
    QVector<NovaTerm::RenderCommand> contents;
    int generatedColumns = 0;
    NovaTerm::mergeRowCommandsIncremental(
        oldBackgrounds, oldContents, 6, spans, false,
        [&generatedColumns](int column,
                            QVector<NovaTerm::RenderCommand>& outBackgrounds,
                            QVector<NovaTerm::RenderCommand>& outContents) {
            ++generatedColumns;
            outBackgrounds.push_back(commandAtColumn(column, 10 + column, false));
            outContents.push_back(commandAtColumn(column, 20 + column, false));
            outContents.push_back(commandAtColumn(column, 30 + column, true));
        },
        backgrounds, contents);

    const auto isAscending = [](const QVector<NovaTerm::RenderCommand>& commands) {
        for (int index = 1; index < commands.size(); ++index) {
            if (commands[index].cellColumn < commands[index - 1].cellColumn)
                return false;
        }
        return true;
    };
    // 只为脏列生成一次，且结果按列升序（等价于旧实现的 stable_sort）。
    QCOMPARE(generatedColumns, 2);
    QVERIFY(isAscending(backgrounds));
    QVERIFY(isAscending(contents));

    // 背景：行级命令 + 6 列；列 2、3 换成新生成，其余保留旧命令。
    QCOMPARE(backgrounds.size(), 7);
    QCOMPARE(backgrounds[0].cellColumn, -1);
    QCOMPARE(backgrounds[0].atlasPage, 99);
    QCOMPARE(backgrounds[1].cellColumn, 0);
    QCOMPARE(backgrounds[2].cellColumn, 1);
    QCOMPARE(backgrounds[2].atlasPage, 1);
    QCOMPARE(backgrounds[3].cellColumn, 2);
    QCOMPARE(backgrounds[3].atlasPage, 12);
    QCOMPARE(backgrounds[4].cellColumn, 3);
    QCOMPARE(backgrounds[4].atlasPage, 13);
    QCOMPARE(backgrounds[5].cellColumn, 4);
    QCOMPARE(backgrounds[5].atlasPage, 1);
    QCOMPARE(backgrounds[6].cellColumn, 5);

    // 内容：列 0/1/4/5 的旧命令（列 4 含下划线）+ 列 2/3 的新命令。
    QCOMPARE(contents.size(), 9);
    QCOMPARE(contents[0].cellColumn, 0);
    QCOMPARE(contents[1].cellColumn, 1);
    QCOMPARE(contents[2].cellColumn, 2);
    QCOMPARE(contents[2].atlasPage, 22);
    QCOMPARE(contents[3].cellColumn, 2);
    QCOMPARE(contents[3].atlasPage, 32);
    QVERIFY(contents[3].colorGlyph);
    QCOMPARE(contents[4].cellColumn, 3);
    QCOMPARE(contents[5].cellColumn, 3);
    QCOMPARE(contents[6].cellColumn, 4);
    QCOMPARE(contents[6].atlasPage, 2);
    QCOMPARE(contents[7].cellColumn, 4);
    QCOMPARE(contents[7].atlasPage, 3);
    QCOMPARE(contents[8].cellColumn, 5);

    // 整行重建（replaceAll）：旧命令全部丢弃，只保留本次生成结果。
    QVector<NovaTerm::RenderCommand> replacedBackgrounds;
    QVector<NovaTerm::RenderCommand> replacedContents;
    NovaTerm::mergeRowCommandsIncremental(
        oldBackgrounds, oldContents, 6, {}, true,
        [](int column, QVector<NovaTerm::RenderCommand>& outBackgrounds,
           QVector<NovaTerm::RenderCommand>&) {
            outBackgrounds.push_back(commandAtColumn(column, 40, false));
        },
        replacedBackgrounds, replacedContents);
    QCOMPARE(replacedBackgrounds.size(), 6);
    QVERIFY(replacedContents.isEmpty());
    for (int column = 0; column < 6; ++column) {
        QCOMPARE(replacedBackgrounds[column].cellColumn, column);
        QCOMPARE(replacedBackgrounds[column].atlasPage, 40);
    }

    // 列宽之外的旧命令（缩窄终端后遗留）不能被静默丢弃。
    QVector<NovaTerm::RenderCommand> wideBackgrounds;
    QVector<NovaTerm::RenderCommand> wideContents;
    NovaTerm::mergeRowCommandsIncremental(
        oldBackgrounds, oldContents, 3, {{1, 2}}, false,
        [](int, QVector<NovaTerm::RenderCommand>&,
           QVector<NovaTerm::RenderCommand>&) {},
        wideBackgrounds, wideContents);
    // 6 条背景：行级 + 列 0/2（保留）+ 列 3/4/5（列宽之外，追加在末尾）。
    QCOMPARE(wideBackgrounds.size(), 6);
    QCOMPARE(wideBackgrounds[0].cellColumn, -1);
    QCOMPARE(wideBackgrounds[1].cellColumn, 0);
    QCOMPARE(wideBackgrounds[2].cellColumn, 2);
    QCOMPARE(wideBackgrounds[3].cellColumn, 3);
    QCOMPARE(wideBackgrounds[4].cellColumn, 4);
    QCOMPARE(wideBackgrounds[5].cellColumn, 5);
}

void RendererP3Tests::mutableRowRebuildKeepsMetadataAndDropsOutOfRange()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(2, 4);
    const int rowsBefore = buffer.rows();

    NovaTerm::RenderCommandRow& row = buffer.mutableRow(1);
    row.backgrounds.push_back(commandAtColumn(0, 1, false));
    buffer.finishRow(1, 42);
    QCOMPARE(buffer.row(1).backgrounds.size(), 1);
    QCOMPARE(buffer.row(1).atlasGeneration, quint64(42));

    // 越界写入必须落到 scratch 并被丢弃，而不是扩容或崩溃。
    NovaTerm::RenderCommandRow& outOfRange = buffer.mutableRow(rowsBefore + 5);
    outOfRange.backgrounds.push_back(commandAtColumn(0, 7, false));
    buffer.finishRow(rowsBefore + 5, 42);
    QCOMPARE(buffer.rows(), rowsBefore);
    QCOMPARE(buffer.row(1).backgrounds.size(), 1);
    QCOMPARE(buffer.row(0).backgrounds.size(), 0);
}

void RendererP3Tests::schedulerMergesTouchingRegions()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({2, 4, 3, 8});
    scheduler.schedule({4, 6, 7, 12});

    QVERIFY(spy.wait(100));
    QCOMPARE(spy.size(), 1);
    const auto arguments = spy.takeFirst();
    const auto regions =
        qvariant_cast<QVector<NovaTerm::DirtyRegion>>(arguments.at(0));
    QCOMPARE(regions.size(), 1);
    QCOMPARE(regions[0].startRow, 2);
    QCOMPARE(regions[0].endRow, 6);
    QCOMPARE(regions[0].startColumn, 3);
    QCOMPARE(regions[0].endColumn, 12);
    QCOMPARE(arguments.at(1).toBool(), false);
}

void RendererP3Tests::schedulerDoesNotMergeCornerOnlyRegions()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({2, 4, 3, 8});
    scheduler.schedule({4, 6, 8, 12});

    QVERIFY(spy.wait(100));
    const auto arguments = spy.takeFirst();
    const auto regions =
        qvariant_cast<QVector<NovaTerm::DirtyRegion>>(arguments.at(0));
    QCOMPARE(regions.size(), 2);
}

void RendererP3Tests::schedulerClipsAndIgnoresEmptyRegions()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({-5, 2, -10, 3});
    scheduler.schedule({30, 40, 0, 5});
    scheduler.schedule({5, 5, 1, 2});

    QVERIFY(spy.wait(100));
    const auto arguments = spy.takeFirst();
    const auto regions =
        qvariant_cast<QVector<NovaTerm::DirtyRegion>>(arguments.at(0));
    QCOMPARE(regions.size(), 1);
    QCOMPARE(regions[0].startRow, 0);
    QCOMPARE(regions[0].endRow, 2);
    QCOMPARE(regions[0].startColumn, 0);
    QCOMPARE(regions[0].endColumn, 3);
    QCOMPARE(scheduler.statistics().dirtyRegionsReceived, quint64(1));
}

void RendererP3Tests::schedulerPromotesLargeDamage()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({0, 20, 0, 80});

    QVERIFY(spy.wait(100));
    const auto arguments = spy.takeFirst();
    QCOMPARE(arguments.at(1).toBool(), true);
    QCOMPARE(scheduler.statistics().fullFrames, quint64(1));
}

void RendererP3Tests::schedulerPromotesAtExactCoverageThreshold()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(10, 10);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({0, 6, 0, 10}, 1);

    QVERIFY(spy.wait(100));
    QCOMPARE(spy.first().at(1).toBool(), true);
}

void RendererP3Tests::schedulerPromotesTooManyRegions()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(100, 100);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    for (int index = 0; index < 33; ++index) {
        const int row = (index / 10) * 3;
        const int column = (index % 10) * 3;
        scheduler.schedule({row, row + 1, column, column + 1});
    }

    QVERIFY(spy.wait(100));
    const auto arguments = spy.takeFirst();
    QCOMPARE(arguments.at(1).toBool(), true);
}

void RendererP3Tests::schedulerCoalescesFrameRequests()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    for (int column = 0; column < 10; ++column)
        scheduler.schedule({0, 1, column, column + 1});

    QVERIFY(spy.wait(100));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(scheduler.statistics().framesRequested, quint64(1));
    QVERIFY(scheduler.statistics().coalescedFrameRequests >= 9);
}

void RendererP3Tests::schedulerSubmitsOverlayOnlyFrame()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.scheduleOverlay();

    QVERIFY(spy.wait(100));
    const auto arguments = spy.takeFirst();
    const auto regions =
        qvariant_cast<QVector<NovaTerm::DirtyRegion>>(arguments.at(0));
    QVERIFY(regions.isEmpty());
    QCOMPARE(arguments.at(1).toBool(), false);
    QCOMPARE(arguments.at(2).toBool(), true);
    QCOMPARE(arguments.at(3).toULongLong(), quint64(0));
}

void RendererP3Tests::schedulerPublishesNewestContentRevision()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({0, 1, 0, 1}, 41);
    scheduler.schedule({1, 2, 0, 1}, 43);

    QVERIFY(spy.wait(100));
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(3).toULongLong(), quint64(43));
}

void RendererP3Tests::schedulerCancelDropsContentRevision()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({0, 1, 0, 1}, 99);
    scheduler.cancel();
    scheduler.scheduleOverlay();

    QVERIFY(spy.wait(100));
    QCOMPARE(spy.first().at(3).toULongLong(), quint64(0));
}

void RendererP3Tests::schedulerFullFrameDominatesLaterDamage()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setViewport(80, 24);
    scheduler.cancel();
    QSignalSpy spy(&scheduler, &NovaTerm::RenderScheduler::frameRequested);

    scheduler.schedule({0, 1, 0, 1}, 10);
    scheduler.scheduleFullFrame(11);
    scheduler.schedule({2, 3, 2, 3}, 12);

    QVERIFY(spy.wait(100));
    const auto arguments = spy.takeFirst();
    const auto regions =
        qvariant_cast<QVector<NovaTerm::DirtyRegion>>(arguments.at(0));
    QVERIFY(regions.isEmpty());
    QCOMPARE(arguments.at(1).toBool(), true);
    QCOMPARE(arguments.at(3).toULongLong(), quint64(12));
}

void RendererP3Tests::schedulerRejectsUnsupportedRefreshRate()
{
    NovaTerm::RenderScheduler scheduler;
    scheduler.setTargetRefreshRate(144);
    QCOMPARE(scheduler.targetRefreshRate(), 144);
    scheduler.setTargetRefreshRate(75);
    QCOMPARE(scheduler.targetRefreshRate(), 60);
}

void RendererP3Tests::commandBufferReplacesOnlyDirtyRow()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(3, 80);
    NovaTerm::RenderCommand command;
    command.type = NovaTerm::RenderCommandType::GlyphInstance;

    buffer.replaceRow(0, {}, {command});
    buffer.replaceRow(1, {}, {command});
    const quint64 firstRowRevision = buffer.row(0).revision;
    const quint64 secondRowRevision = buffer.row(1).revision;

    buffer.replaceRow(1, {}, {});

    QCOMPARE(buffer.row(0).revision, firstRowRevision);
    QVERIFY(buffer.row(1).revision > secondRowRevision);
    QCOMPARE(buffer.row(0).contents.size(), 1);
    QVERIFY(buffer.row(1).contents.isEmpty());
}

void RendererP3Tests::commandBufferResizeInvalidatesRows()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(2, 80);
    NovaTerm::RenderCommand command;
    buffer.replaceRow(0, {command}, {});
    QCOMPARE(buffer.commandCount(), qsizetype(1));

    buffer.resize(4, 100);

    QCOMPARE(buffer.rows(), 4);
    QCOMPARE(buffer.columns(), 100);
    QCOMPARE(buffer.commandCount(), qsizetype(0));
}

void RendererP3Tests::commandRowsTrackAtlasGeneration()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(2, 80);
    NovaTerm::RenderCommand glyph;
    glyph.type = NovaTerm::RenderCommandType::GlyphInstance;

    buffer.replaceRow(0, {}, {glyph}, 7);
    buffer.replaceRow(1, {}, {glyph}, 8);

    QCOMPARE(buffer.row(0).atlasGeneration, quint64(7));
    QCOMPARE(buffer.row(1).atlasGeneration, quint64(8));
    buffer.resize(3, 80);
    QCOMPARE(buffer.row(0).atlasGeneration, quint64(0));
}

void RendererP3Tests::commandBufferValidatesAtlasGeneration()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(2, 80);
    buffer.replaceRow(0, {}, {}, 4);
    buffer.replaceRow(1, {}, {}, 4);
    QVERIFY(buffer.rowsUseAtlasGeneration(4));

    buffer.replaceRow(1, {}, {}, 5);
    QVERIFY(!buffer.rowsUseAtlasGeneration(4));
    QVERIFY(!buffer.rowsUseAtlasGeneration(5));
}

void RendererP3Tests::scrollbackAtLiveBottomDoesNotRequestFullFrame()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QTest::qWait(30);
    const quint64 fullFramesBefore =
        renderer.renderStatistics().scheduler.fullFrames;

    emit core.scrollbackChanged();
    QTest::qWait(30);

    QCOMPARE(renderer.renderStatistics().scheduler.fullFrames,
             fullFramesBefore);
}

// 布局常驻：实时底部也维护历史布局，且新输出只做增量追加 —— 不再为每批
// 输出发起一次全量重排（旧实现在底部丢弃布局，行数因此退化成逻辑行数）。
void RendererP3Tests::liveBottomMaintainsHistoryLayout()
{
    TerminalCore core(80, 6);
    TerminalRenderer renderer(&core);
    QTest::qWait(80);

    QByteArray input;
    for (int i = 0; i < 100; ++i)
        input += QByteArrayLiteral("live-bottom\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(core.scrollbackLineCount() > 0, 1000);

    // 首次建立布局走一次异步全量重排，之后布局必须常驻可用。
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);
    QCOMPARE(renderer.scrollOffset(), 0);
    const quint64 requestsAfterFirstBuild =
        renderer.renderStatistics().scrollbackReflowRequests;

    // 继续输出：列宽没变，只应增量追加，不得再发全量重排请求。
    QByteArray more;
    for (int i = 0; i < 50; ++i)
        more += QByteArrayLiteral("more-output\r\n");
    QVERIFY(core.writeInput(more).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTest::qWait(80);

    QCOMPARE(renderer.renderStatistics().scrollbackReflowRequests,
             requestsAfterFirstBuild);
    // 增量维护必须把新行折进布局，行数随之增长。
    QCOMPARE(renderer.historyDisplayRowCount(),
             qsizetype(core.scrollbackLineCount()));
}

void RendererP3Tests::batchedScreenScrollPublishesExactRowCount()
{
    TerminalCore core(80, 6);
    QSignalSpy scrollSpy(&core, &TerminalCore::screenScrolled);

    QByteArray input;
    for (int line = 0; line < 20; ++line)
        input += QByteArrayLiteral("line\r\n");
    const auto result = core.writeInput(input);
    QVERIFY(result.fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(!scrollSpy.isEmpty(), 1000);

    int publishedRows = 0;
    for (const auto& arguments : scrollSpy)
        publishedRows += arguments.at(0).toInt();
    QCOMPARE(publishedRows, core.scrollbackLineCount());
    QVERIFY(publishedRows > 1);
}

// 进入历史不再触发重排：布局已常驻且与当前列宽一致，直接可用。
void RendererP3Tests::enteringHistoryReusesHistoryLayout()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 30; ++i)
        input += QByteArrayLiteral("history\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(core.scrollbackLineCount() > 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);

    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;
    renderer.scrollLines(1);
    QCOMPARE(renderer.scrollOffset(), 1);

    QTest::qWait(80);
    QCOMPARE(renderer.renderStatistics().scrollbackReflowRequests,
             requestsBefore);
}

// 回到实时底部保留布局：不再丢弃，因此行数始终可用于滚动条量程，再次
// 进入历史也无需等待一次重建。
void RendererP3Tests::returningToLiveBottomKeepsHistoryLayout()
{
    TerminalCore core(80, 6);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 100; ++i)
        input += QByteArrayLiteral("history\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(core.scrollbackLineCount() > 0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);

    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;
    const qsizetype rowsBefore = renderer.historyDisplayRowCount();

    renderer.scrollLines(1);
    QCOMPARE(renderer.scrollOffset(), 1);
    renderer.scrollLines(-1);
    QCOMPARE(renderer.scrollOffset(), 0);
    QTest::qWait(80);
    QCOMPARE(renderer.historyDisplayRowCount(), rowsBefore);

    renderer.scrollLines(1);
    QCOMPARE(renderer.scrollOffset(), 1);
    renderer.scrollToBottom();
    QCOMPARE(renderer.scrollOffset(), 0);
    QTest::qWait(80);
    QCOMPARE(renderer.historyDisplayRowCount(), rowsBefore);
    QCOMPARE(renderer.renderStatistics().scrollbackReflowRequests,
             requestsBefore);
}

// 重排判据是**列数**而非尺寸：行数变化不影响折行，不应触发重排。
// 直接驱动 TerminalCore::resize 而不用 QWidget::resize —— 测试里的渲染器
// 从未 show，widget 几何变化不会传导到终端尺寸。
void RendererP3Tests::columnChangeRequestsReflowRowChangeDoesNot()
{
    TerminalCore core(80, 6);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 80; ++i)
        input += QByteArrayLiteral("resize-probe\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);

    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;

    // 只改行数，随后用一次输出触发 scrollbackChanged 让布局做一致性检查。
    core.resize(80, 10);
    QVERIFY(core.waitForIdle(1000));
    QVERIFY(core.writeInput(QByteArrayLiteral("after-rows\r\n"))
                .fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTest::qWait(80);
    QCOMPARE(renderer.renderStatistics().scrollbackReflowRequests,
             requestsBefore);

    // 改列数：折点全部位移，必须重排。
    core.resize(40, 10);
    QVERIFY(core.waitForIdle(1000));
    QVERIFY(core.writeInput(QByteArrayLiteral("after-cols\r\n"))
                .fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(
        renderer.renderStatistics().scrollbackReflowRequests > requestsBefore,
        2000);
}

namespace {

// 找到 documentRow 对应的一个 widget y 坐标。_cellHeight 是私有的，改用
// 公开的 widgetToCell 反查，避免测试依赖内部字体度量。
int widgetYForDocumentRow(const TerminalRenderer& renderer, int documentRow)
{
    for (int y = 0; y < renderer.height(); ++y) {
        if (renderer.widgetToCell(QPoint(0, y)).y() == documentRow)
            return y;
    }
    return -1;
}

} // namespace

// updateHistoryLayout() 每次 scrollbackChanged（每批输出）曾调全量
// scrollbackSnapshot()，而 ChunkedScrollback::snapshot() 会 publish() →
// sealActive()，把未填满的 active 块封存成小分块 —— 分块数从「行数/1024」
// 退化成「发布批次数」，并每批复制全部 ChunkView。
//
// 优化后（尾部增量窄接口 scrollbackTail/tailFrom）增量路径不再取全量快照，
// 碎片化被消除：分块数回到由历史行数决定、与发布批次数无关。
//
// 本测试即此优化的验收：同样内容拆成多批产生的 sealedChunks 应与单批写入
// 同量级（而非 ≈ 批次数）；字节记账的膨胀同时保持可忽略。
void RendererP3Tests::fragmentedOutputDoesNotInflateScrollbackBytes()
{
    struct Result
    {
        qsizetype bytes{0};
        qsizetype chunks{0};
        qsizetype lines{0};
    };

    const auto produce = [](int batches, int linesPerBatch, Result* out) {
        TerminalCore core(80, 24);
        core.setScrollbackLimit(100000);
        TerminalRenderer renderer(&core);
        QVERIFY(core.waitForIdle(2000));
        for (int batch = 0; batch < batches; ++batch) {
            QByteArray input;
            for (int i = 0; i < linesPerBatch; ++i)
                input += QByteArrayLiteral("fragmentation-probe-line\r\n");
            QVERIFY(core.writeInput(input).fullyAccepted());
            QVERIFY(core.waitForIdle(10000));
            // 等显示布局追上历史行数：首建走一次全量重排，之后每批走尾部增量。
            // 探针行 24 字符 < 80 列、无软换行，故显示行数应等于逻辑行数。这样
            // 复现的是优化后的稳定分块行为，而非首建期的反复重排。
            QTRY_VERIFY_WITH_TIMEOUT(
                renderer.historyDisplayRowCount()
                    == qsizetype(core.scrollbackLineCount()),
                5000);
        }
        QTest::qWait(50);
        const auto statistics = core.scrollbackStatistics();
        out->bytes = statistics.effectiveBytes;
        out->chunks = statistics.sealedChunks;
        out->lines = statistics.logicalLines;
    };

    Result single;
    Result fragmented;
    produce(1, 2000, &single);
    produce(100, 20, &fragmented);

    // 两种写法产生同样的内容。
    QCOMPARE(fragmented.lines, single.lines);
    QVERIFY(single.lines > 1000);
    // 碎片化被消除：分块数由历史行数（≈行数/1024）决定，不随发布批次数增长。
    // 优化前增量路径每批 snapshot()→sealActive()，fragmented.chunks 会 ≈ 批次数
    // （100）；优化后应与单批写入同量级。
    QVERIFY2(fragmented.chunks <= single.chunks + 3,
             qPrintable(QStringLiteral(
                 "fragmentation not eliminated: single=%1 chunks, fragmented=%2 "
                 "chunks (should be same order, not ~batch count)")
                            .arg(single.chunks)
                            .arg(fragmented.chunks)));
    // 字节记账的膨胀必须可忽略。
    QVERIFY2(fragmented.bytes <= single.bytes + single.bytes / 20,
             qPrintable(QStringLiteral(
                 "chunk accounting overhead inflated scrollback bytes: "
                 "single=%1 fragmented=%2 chunks %3 -> %4")
                            .arg(single.bytes)
                            .arg(fragmented.bytes)
                            .arg(single.chunks)
                            .arg(fragmented.chunks)));
}

void RendererP3Tests::softWrappedSelectionCopiesAsSingleLine()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QVERIFY(core.waitForIdle(1000));
    QTest::qWait(30);

    const int columns = core.columns();
    const int total = columns + 20;
    QVERIFY(core.writeInput(QByteArray(total, 'A')).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    // 第 1 行是第 0 行的软换行延续。
    QTRY_VERIFY_WITH_TIMEOUT(core.rowContinuation(1), 1000);

    const int yRow0 = widgetYForDocumentRow(renderer, 0);
    const int yRow1 = widgetYForDocumentRow(renderer, 1);
    QVERIFY(yRow0 >= 0);
    QVERIFY(yRow1 > yRow0);

    // 注意：QPoint(0, 0) 是 null QPoint，QTest 会把它当作"控件中心"。起点
    // 必须用非 null 坐标，否则选区会从屏幕中间开始。
    // 只发 press+move 不发 release —— release 会自动写剪贴板。
    QTest::mousePress(&renderer, Qt::LeftButton, {}, QPoint(1, yRow0 + 1));
    QTest::mouseMove(&renderer, QPoint(renderer.width() - 1, yRow1));
    QVERIFY(renderer.hasSelection());

    const QString text = renderer.selectedText();
    QVERIFY2(!text.contains(QLatin1Char('\n')),
             "soft-wrapped selection must not contain a newline");
    QVERIFY(text.startsWith(QString(total, QLatin1Char('A'))));
}

// 对照：跨真实换行的选区仍必须保留换行。
void RendererP3Tests::hardBreakSelectionKeepsNewline()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QVERIFY(core.waitForIdle(1000));
    QTest::qWait(30);

    QVERIFY(core.writeInput(QByteArrayLiteral("AAA\r\nBBB")).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QVERIFY(!core.rowContinuation(1));

    const int yRow0 = widgetYForDocumentRow(renderer, 0);
    const int yRow1 = widgetYForDocumentRow(renderer, 1);
    QVERIFY(yRow0 >= 0);
    QVERIFY(yRow1 > yRow0);

    QTest::mousePress(&renderer, Qt::LeftButton, {}, QPoint(1, yRow0 + 1));
    QTest::mouseMove(&renderer, QPoint(renderer.width() - 1, yRow1));
    QVERIFY(renderer.hasSelection());

    const QString text = renderer.selectedText();
    QCOMPARE(text.count(QLatin1Char('\n')), 1);
    QVERIFY(text.startsWith(QStringLiteral("AAA")));
}

void RendererP3Tests::searchMatchesAppendByGeneration()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.appendSearchMatches({{1, 0, 1}, {1, 2, 3}}, 10);
    renderer.appendSearchMatches({{2, 0, 1}}, 10);
    QCOMPARE(renderer.searchMatchCount(), qsizetype(3));
    QCOMPARE(renderer.searchMatchedLineCount(), qsizetype(2));

    renderer.appendSearchMatches({{3, 0, 1}}, 9);
    QCOMPARE(renderer.searchMatchCount(), qsizetype(3));

    renderer.appendSearchMatches({{4, 0, 1}}, 11);
    QCOMPARE(renderer.searchMatchCount(), qsizetype(1));
    QCOMPARE(renderer.searchMatchedLineCount(), qsizetype(1));
}

void RendererP3Tests::inputMethodCommitProducesUtf8()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QVERIFY(renderer.testAttribute(Qt::WA_InputMethodEnabled));
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);

    QInputMethodEvent event;
    const QString committed = QString::fromUtf8(u8"中文🌟");
    event.setCommitString(committed);
    QCoreApplication::sendEvent(&renderer, &event);

    QVERIFY(event.isAccepted());
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!outputSpy.isEmpty());

    QByteArray output;
    for (const auto& arguments : outputSpy)
        output += arguments.at(0).toByteArray();
    QCOMPARE(output, committed.toUtf8());
}

QTEST_MAIN(RendererP3Tests)
#include "RendererP3Tests.moc"
