#include "renderer/RenderCommandBuffer.h"
#include "renderer/RenderScheduler.h"
#include "renderer/TerminalRenderer.h"
#include "core/terminal/TerminalCore.h"

#include <QSignalSpy>
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
    void liveBottomDefersScrollbackReflow();
    void enteringHistoryRequestsScrollbackReflow();
    void searchMatchesAppendByGeneration();
};

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

void RendererP3Tests::liveBottomDefersScrollbackReflow()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QTest::qWait(80);
    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;

    for (int i = 0; i < 100; ++i)
        emit core.scrollbackChanged();

    QTest::qWait(80);
    QCOMPARE(renderer.renderStatistics().scrollbackReflowRequests,
             requestsBefore);
}

void RendererP3Tests::enteringHistoryRequestsScrollbackReflow()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 30; ++i)
        input += QByteArrayLiteral("history\r\n");
    core.writeInput(input);
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(core.scrollbackLineCount() > 0, 1000);

    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;
    renderer.scrollLines(1);

    QTRY_VERIFY_WITH_TIMEOUT(
        renderer.renderStatistics().scrollbackReflowRequests
            > requestsBefore,
        1000);
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

QTEST_MAIN(RendererP3Tests)
#include "RendererP3Tests.moc"
