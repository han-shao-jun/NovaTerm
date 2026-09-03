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

// 超宽输出被自动换行成的多个屏幕行属于同一逻辑行，复制必须拼回一行。
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
