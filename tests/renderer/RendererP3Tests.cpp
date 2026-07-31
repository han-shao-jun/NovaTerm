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
    void schedulerPromotesTooManyRegions();
    void schedulerCoalescesFrameRequests();
    void schedulerSubmitsOverlayOnlyFrame();
    void commandBufferReplacesOnlyDirtyRow();
    void commandBufferResizeInvalidatesRows();
    void scrollbackAtLiveBottomDoesNotRequestFullFrame();
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

QTEST_MAIN(RendererP3Tests)
#include "RendererP3Tests.moc"
