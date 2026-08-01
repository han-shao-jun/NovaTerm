#include "core/scrollback/ChunkedScrollback.h"
#include "core/scrollback/LineLayout.h"
#include "core/search/SearchEngine.h"

#include <QSignalSpy>
#include <QElapsedTimer>
#include <QtTest>

class ScrollbackTests final : public QObject
{
    Q_OBJECT

private slots:
    void chunkEvictionKeepsSnapshotsStable();
    void enforcesLineAndByteBudgets();
    void snapshotUsesStableLineIds();
    void layoutKeepsWideCellsTogether();
    void reflowPublishesCurrentGeneration();
    void searchPublishesCellRanges();
    void searchCancellationSupersedesGeneration();
    void activeTailSnapshotIsPublishedWithoutCellCopy();
    void retainedMemoryFallsAfterSnapshotRelease();
    void zeroBudgetsAndOversizedLineEvictImmediately();
    void liveSnapshotSurvivesClearAndLimitChanges();
    void staleGenerationsAreRejected();
    void unicodeSearchMapsUtf16BackToCells();
    void regexGuardsAndResultLimitAreEnforced();
    void destroyingBusyWorkersIsBounded();
};

namespace {

NovaTerm::LogicalLine textLine(const QString& text)
{
    NovaTerm::LogicalLine line;
    for (uint codepoint : text.toUcs4()) {
        NovaTerm::Cell cell;
        cell.chars[0] = codepoint;
        cell.width = 1;
        line.cells.push_back(cell);
    }
    return line;
}

} // namespace

void ScrollbackTests::chunkEvictionKeepsSnapshotsStable()
{
    NovaTerm::ChunkedScrollback scrollback(2, 1024 * 1024, 2);
    scrollback.append(textLine(QStringLiteral("A")));
    scrollback.append(textLine(QStringLiteral("B")));
    const NovaTerm::ScrollbackSnapshot before = scrollback.snapshot();

    scrollback.append(textLine(QStringLiteral("C")));
    scrollback.append(textLine(QStringLiteral("D")));
    const NovaTerm::ScrollbackSnapshot after = scrollback.snapshot();

    QCOMPARE(before.lineCount(), qsizetype(2));
    QCOMPARE(before.lineAt(0)->cells[0].chars[0], uint32_t('A'));
    QCOMPARE(after.lineCount(), qsizetype(2));
    QCOMPARE(after.lineAt(0)->cells[0].chars[0], uint32_t('C'));
    QVERIFY(scrollback.statistics().retainedBySnapshots > 0);
}

void ScrollbackTests::enforcesLineAndByteBudgets()
{
    const qsizetype oneLine = textLine(QStringLiteral("budget")).byteSize();
    NovaTerm::ChunkedScrollback scrollback(100, oneLine * 2, 4);
    for (int i = 0; i < 5; ++i)
        scrollback.append(textLine(QStringLiteral("budget")));
    QVERIFY(scrollback.lineCount() <= 2);
    QVERIFY(scrollback.statistics().effectiveBytes <= oneLine * 2);

    scrollback.setLimits(1, oneLine * 2);
    QCOMPARE(scrollback.lineCount(), qsizetype(1));
}

void ScrollbackTests::snapshotUsesStableLineIds()
{
    NovaTerm::ChunkedScrollback scrollback(3, 1024 * 1024, 2);
    const NovaTerm::LineId first = scrollback.append(textLine("first"));
    const NovaTerm::LineId second = scrollback.append(textLine("second"));
    QCOMPARE(scrollback.snapshot().rowForLineId(second), qsizetype(1));

    scrollback.append(textLine("third"));
    const NovaTerm::LineId fourth = scrollback.append(textLine("fourth"));
    const auto snapshot = scrollback.snapshot();
    QVERIFY(!snapshot.contains(first));
    QCOMPARE(snapshot.rowForLineId(second), qsizetype(0));
    QCOMPARE(snapshot.rowForLineId(fourth), qsizetype(2));
}

void ScrollbackTests::layoutKeepsWideCellsTogether()
{
    NovaTerm::LogicalLine line;
    line.id = 7;
    line.cells.resize(4);
    line.cells[0].chars[0] = 'A';
    line.cells[1].chars[0] = 0x4e2d;
    line.cells[1].width = 2;
    line.cells[2].chars[0] = NovaTerm::WideCharContinuation;
    line.cells[2].width = 1;
    line.cells[3].chars[0] = 'B';

    const QVector<NovaTerm::DisplayLine> rows =
        NovaTerm::LineLayout::wrapLine(line, 2);
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].startCell, qsizetype(0));
    QCOMPARE(rows[0].endCell, qsizetype(1));
    QCOMPARE(rows[1].startCell, qsizetype(1));
    QCOMPARE(rows[1].endCell, qsizetype(3));
    QCOMPARE(rows[2].startCell, qsizetype(3));
    QVERIFY(rows[2].hardBreak);
    line.hardBreak = false;
    const auto softRows = NovaTerm::LineLayout::wrapLine(line, 2);
    for (const auto& row : softRows)
        QVERIFY(!row.hardBreak);
}

void ScrollbackTests::reflowPublishesCurrentGeneration()
{
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 4);
    for (int i = 0; i < 20; ++i)
        scrollback.append(textLine(QStringLiteral("abcdefgh")));

    NovaTerm::ReflowEngine reflow;
    QSignalSpy spy(&reflow, &NovaTerm::ReflowEngine::batchReady);
    reflow.request(scrollback.snapshot(), 4, 1, 3);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::ReflowBatch>().completed, 3000);
    const auto batch = spy.last().at(0).value<NovaTerm::ReflowBatch>();
    QCOMPARE(batch.generation, quint64(1));
    QCOMPARE(batch.physicalRows, qsizetype(40));
}

void ScrollbackTests::searchPublishesCellRanges()
{
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 4);
    scrollback.append(textLine(QStringLiteral("hello world")));
    const NovaTerm::LineId matchLine =
        scrollback.append(textLine(QStringLiteral("another hello")));

    NovaTerm::SearchEngine search;
    QSignalSpy spy(&search, &NovaTerm::SearchEngine::resultsReady);
    NovaTerm::SearchRequest request;
    request.query = QStringLiteral("hello");
    request.generation = 1;
    request.resultBatchSize = 1;
    search.search(scrollback.snapshot(), request);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::SearchBatch>().completed, 3000);

    QVector<NovaTerm::SearchMatch> matches;
    for (const QList<QVariant>& arguments : spy)
        matches += arguments.at(0).value<NovaTerm::SearchBatch>().matches;
    QCOMPARE(matches.size(), 2);
    QCOMPARE(matches[1].lineId, matchLine);
    QCOMPARE(matches[1].startCell, qsizetype(8));
    QCOMPARE(matches[1].endCell, qsizetype(13));
}

void ScrollbackTests::searchCancellationSupersedesGeneration()
{
    NovaTerm::ChunkedScrollback scrollback(10'000, 64 * 1024 * 1024, 64);
    for (int i = 0; i < 5'000; ++i)
        scrollback.append(textLine(QStringLiteral("generation test line")));

    NovaTerm::SearchEngine search;
    QSignalSpy spy(&search, &NovaTerm::SearchEngine::resultsReady);
    NovaTerm::SearchRequest oldRequest;
    oldRequest.query = QStringLiteral("test");
    oldRequest.generation = 1;
    search.search(scrollback.snapshot(), oldRequest);

    NovaTerm::SearchRequest currentRequest;
    currentRequest.query = QStringLiteral("missing");
    currentRequest.generation = 2;
    search.search(scrollback.snapshot(), currentRequest);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::SearchBatch>().generation == 2
        && spy.last().at(0).value<NovaTerm::SearchBatch>().completed, 5000);
}

void ScrollbackTests::activeTailSnapshotIsPublishedWithoutCellCopy()
{
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 16);
    scrollback.append(textLine(QStringLiteral("tail")));
    const auto first = scrollback.snapshot();
    QCOMPARE(first.chunks().size(), 1);
    QCOMPARE(scrollback.statistics().activeLines, qsizetype(0));
    QCOMPARE(scrollback.statistics().sealedChunks, qsizetype(1));

    scrollback.append(textLine(QStringLiteral("new tail")));
    const auto second = scrollback.snapshot();
    QCOMPARE(second.chunks().size(), 2);
    QCOMPARE(first.chunks().front().chunk.get(),
             second.chunks().front().chunk.get());
    QCOMPARE(first.version(), quint64(1));
    QCOMPARE(second.version(), quint64(2));
}

void ScrollbackTests::retainedMemoryFallsAfterSnapshotRelease()
{
    NovaTerm::ChunkedScrollback scrollback(2, 1024 * 1024, 2);
    scrollback.append(textLine(QStringLiteral("A")));
    scrollback.append(textLine(QStringLiteral("B")));
    {
        const auto snapshot = scrollback.snapshot();
        scrollback.append(textLine(QStringLiteral("C")));
        scrollback.append(textLine(QStringLiteral("D")));
        QVERIFY(scrollback.statistics().retainedBySnapshots > 0);
        QCOMPARE(snapshot.lineAt(0)->cells[0].chars[0], uint32_t('A'));
    }
    QCOMPARE(scrollback.statistics().retainedBySnapshots, qsizetype(0));
}

void ScrollbackTests::zeroBudgetsAndOversizedLineEvictImmediately()
{
    NovaTerm::ChunkedScrollback zeroLines(0, 1024 * 1024, 4);
    zeroLines.append(textLine(QStringLiteral("discard")));
    QCOMPARE(zeroLines.lineCount(), qsizetype(0));

    NovaTerm::ChunkedScrollback zeroBytes(100, 0, 4);
    zeroBytes.append(textLine(QStringLiteral("discard")));
    QCOMPARE(zeroBytes.lineCount(), qsizetype(0));
    QCOMPARE(zeroBytes.statistics().effectiveBytes, qsizetype(0));

    NovaTerm::ChunkedScrollback tooLarge(100, 32, 4);
    tooLarge.append(textLine(QString(4096, QLatin1Char('x'))));
    QCOMPARE(tooLarge.lineCount(), qsizetype(0));
}

void ScrollbackTests::liveSnapshotSurvivesClearAndLimitChanges()
{
    NovaTerm::ChunkedScrollback scrollback(8, 1024 * 1024, 4);
    for (int i = 0; i < 8; ++i)
        scrollback.append(textLine(QString::number(i)));
    const auto snapshot = scrollback.snapshot();
    scrollback.setLimits(1, 1024 * 1024);
    scrollback.clear();
    QCOMPARE(scrollback.lineCount(), qsizetype(0));
    QCOMPARE(snapshot.lineCount(), qsizetype(8));
    QCOMPARE(snapshot.lineAt(7)->cells[0].chars[0], uint32_t('7'));
    QVERIFY(scrollback.statistics().retainedBySnapshots > 0);
}

void ScrollbackTests::staleGenerationsAreRejected()
{
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 8);
    for (int i = 0; i < 32; ++i)
        scrollback.append(textLine(QStringLiteral("generation")));

    NovaTerm::ReflowEngine reflow;
    QSignalSpy reflowSpy(&reflow, &NovaTerm::ReflowEngine::batchReady);
    reflow.request(scrollback.snapshot(), 4, 20, 2);
    reflow.request(scrollback.snapshot(), 8, 19, 2);
    QTRY_VERIFY_WITH_TIMEOUT(!reflowSpy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        reflowSpy.last().at(0).value<NovaTerm::ReflowBatch>().completed, 3000);
    for (const auto& arguments : reflowSpy)
        QVERIFY(arguments.at(0).value<NovaTerm::ReflowBatch>().generation != 19);

    NovaTerm::SearchEngine search;
    QSignalSpy searchSpy(&search, &NovaTerm::SearchEngine::resultsReady);
    NovaTerm::SearchRequest current;
    current.query = QStringLiteral("generation");
    current.generation = 20;
    search.search(scrollback.snapshot(), current);
    NovaTerm::SearchRequest stale = current;
    stale.generation = 19;
    search.search(scrollback.snapshot(), stale);
    QTRY_VERIFY_WITH_TIMEOUT(!searchSpy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        searchSpy.last().at(0).value<NovaTerm::SearchBatch>().completed, 3000);
    for (const auto& arguments : searchSpy)
        QVERIFY(arguments.at(0).value<NovaTerm::SearchBatch>().generation != 19);
}

void ScrollbackTests::unicodeSearchMapsUtf16BackToCells()
{
    NovaTerm::ChunkedScrollback scrollback(10, 1024 * 1024, 4);
    const auto id = scrollback.append(textLine(QString::fromUtf8("A😀中B")));
    NovaTerm::SearchEngine search;
    QSignalSpy spy(&search, &NovaTerm::SearchEngine::resultsReady);
    NovaTerm::SearchRequest request;
    request.query = QString::fromUtf8("😀中");
    request.generation = 1;
    search.search(scrollback.snapshot(), request);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::SearchBatch>().completed, 3000);
    const auto matches = spy.first().at(0).value<NovaTerm::SearchBatch>().matches;
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches[0].lineId, id);
    QCOMPARE(matches[0].startCell, qsizetype(1));
    QCOMPARE(matches[0].endCell, qsizetype(3));
}

void ScrollbackTests::regexGuardsAndResultLimitAreEnforced()
{
    NovaTerm::ChunkedScrollback scrollback(10, 1024 * 1024, 4);
    scrollback.append(textLine(QStringLiteral("aaaa! aaaa!")));
    NovaTerm::SearchEngine search;
    QSignalSpy spy(&search, &NovaTerm::SearchEngine::resultsReady);

    NovaTerm::SearchRequest unsafe;
    unsafe.query = QStringLiteral("(a+)+$");
    unsafe.regularExpression = true;
    unsafe.generation = 1;
    search.search(scrollback.snapshot(), unsafe);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QVERIFY(!spy.last().at(0).value<NovaTerm::SearchBatch>().error.isEmpty());

    NovaTerm::SearchRequest limited;
    limited.query = QStringLiteral("a");
    limited.generation = 2;
    limited.maximumResults = 3;
    limited.resultBatchSize = 2;
    search.search(scrollback.snapshot(), limited);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::SearchBatch>().generation == 2
        && spy.last().at(0).value<NovaTerm::SearchBatch>().completed, 3000);
    qsizetype count = 0;
    for (const auto& arguments : spy) {
        const auto batch = arguments.at(0).value<NovaTerm::SearchBatch>();
        if (batch.generation == 2)
            count += batch.matches.size();
    }
    QCOMPARE(count, qsizetype(3));
}

void ScrollbackTests::destroyingBusyWorkersIsBounded()
{
    NovaTerm::LogicalLine longLine;
    longLine.cells.resize(500'000);
    for (auto& cell : longLine.cells)
        cell.chars[0] = 'a';
    NovaTerm::ChunkedScrollback scrollback(2, 128 * 1024 * 1024, 2);
    scrollback.append(std::move(longLine));
    const auto snapshot = scrollback.snapshot();

    QElapsedTimer timer;
    timer.start();
    {
        NovaTerm::ReflowEngine reflow;
        reflow.request(snapshot, 1, 1, 1);
    }
    QVERIFY2(timer.elapsed() < 2000, "ReflowEngine teardown exceeded 2 seconds");

    timer.restart();
    {
        NovaTerm::SearchEngine search;
        NovaTerm::SearchRequest request;
        request.query = QStringLiteral("missing");
        request.generation = 1;
        search.search(snapshot, request);
    }
    QVERIFY2(timer.elapsed() < 2000, "SearchEngine teardown exceeded 2 seconds");
}

QTEST_GUILESS_MAIN(ScrollbackTests)

#include "ScrollbackTests.moc"
