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
    void continuationAcrossSealedChunkKeepsOneLogicalLine();
    void takeNewestTailAcrossSealedChunkPreservesHistory();
    void sealedChunkStaysSharedWhenItsTailIsRewritten();
    void retainedMemoryFallsAfterSnapshotRelease();
    void zeroBudgetsAndOversizedLineEvictImmediately();
    void liveSnapshotSurvivesClearAndLimitChanges();
    void staleGenerationsAreRejected();
    void unicodeSearchMapsUtf16BackToCells();
    void regexGuardsAndResultLimitAreEnforced();
    void destroyingBusyWorkersIsBounded();
    void tailFromReturnsIncrementalTail();
    void defaultHistoryPreservesOldestPastFormerLineLimit();
    void unlimitedHistoryStillEnforcesByteBudget();
    void unlimitedHistoryCanReplaceExplicitLineLimit();
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

void ScrollbackTests::defaultHistoryPreservesOldestPastFormerLineLimit()
{
    NovaTerm::ChunkedScrollback history;
    const auto first = history.append(textLine(QStringLiteral("oldest")));
    for (int index = 1; index < 100005; ++index)
        history.append(NovaTerm::LogicalLine{});
    const auto snapshot = history.snapshot();
    QCOMPARE(snapshot.lineCount(), NovaTerm::isize(100005));
    QVERIFY(snapshot.lineById(first));
    QCOMPARE(snapshot.lineById(first)->cells[0].chars[0], uint32_t('o'));
    QCOMPARE(history.statistics().evictedLines, NovaTerm::u64(0));
}

void ScrollbackTests::unlimitedHistoryStillEnforcesByteBudget()
{
    NovaTerm::ChunkedScrollback history(-1, 8192, 16);
    const auto first = history.append(textLine(QStringLiteral("budget")));
    for (int index = 0; index < 128; ++index)
        history.append(textLine(QStringLiteral("budget")));
    const auto snapshot = history.snapshot();
    QVERIFY(snapshot.lineCount() > 0);
    QVERIFY(!snapshot.lineById(first));
    QVERIFY(history.statistics().evictedLines > 0);
    QVERIFY(history.statistics().effectiveBytes <= 8192);
}

void ScrollbackTests::unlimitedHistoryCanReplaceExplicitLineLimit()
{
    NovaTerm::ChunkedScrollback history(2, 1024 * 1024, 4);
    history.append(textLine(QStringLiteral("a")));
    const auto retained = history.append(textLine(QStringLiteral("b")));
    history.append(textLine(QStringLiteral("c")));
    history.setLimits(-1, 1024 * 1024);
    history.append(textLine(QStringLiteral("d")));
    history.append(textLine(QStringLiteral("e")));
    const auto snapshot = history.snapshot();
    QCOMPARE(snapshot.lineCount(), NovaTerm::isize(4));
    QVERIFY(snapshot.lineById(retained));
}

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

void ScrollbackTests::tailFromReturnsIncrementalTail()
{
    using NovaTerm::LineId;
    using NovaTerm::ScrollbackTail;

    // 空历史：不 resync，lineCount=0，lines 空。
    {
        NovaTerm::ChunkedScrollback sb(100, 1024 * 1024, 4);
        ScrollbackTail tail;
        sb.tailFrom(1, 4096, tail);
        QVERIFY(!tail.resync);
        QCOMPARE(tail.lineCount, qsizetype(0));
        QVERIFY(tail.lines.empty());
    }

    // 正常增量：返回 [sinceId .. 最新行]，first/last/fromLineId 正确。
    {
        NovaTerm::ChunkedScrollback sb(100, 1024 * 1024, 4);
        const LineId a = sb.append(textLine(QStringLiteral("a")));
        (void)sb.append(textLine(QStringLiteral("b")));
        const LineId c = sb.append(textLine(QStringLiteral("c")));
        const LineId d = sb.append(textLine(QStringLiteral("d")));
        ScrollbackTail tail;
        sb.tailFrom(c, 4096, tail);
        QVERIFY(!tail.resync);
        QCOMPARE(tail.lineCount, qsizetype(4));
        QCOMPARE(tail.firstLineId, a);
        QCOMPARE(tail.lastLineId, d);
        QCOMPARE(tail.fromLineId, c);
        QCOMPARE(tail.lines.size(), std::size_t(2));
        QCOMPARE(tail.lines.front().id, c);
        QCOMPARE(tail.lines.back().id, d);
        QCOMPARE(tail.lines.back().cells[0].chars[0], uint32_t('d'));
    }

    // 尾部跨已封存分块（2 行/块，5 行）：深拷贝仍完整、顺序正确。
    {
        NovaTerm::ChunkedScrollback sb(100, 1024 * 1024, 2);
        LineId ids[5];
        for (int i = 0; i < 5; ++i)
            ids[i] = sb.append(textLine(QString::number(i)));
        ScrollbackTail tail;
        sb.tailFrom(ids[1], 4096, tail);
        QVERIFY(!tail.resync);
        QCOMPARE(tail.lines.size(), std::size_t(4));
        for (int k = 0; k < 4; ++k)
            QCOMPARE(tail.lines[std::size_t(k)].id, ids[1 + k]);
    }

    // sinceId 比现存最新还新（模拟尾行已被 sb_popline 取回）：不 resync，
    // 从现存最新行返回，供调用方删掉被取回行的显示行后重折。
    {
        NovaTerm::ChunkedScrollback sb(100, 1024 * 1024, 4);
        (void)sb.append(textLine(QStringLiteral("x")));
        const LineId last = sb.append(textLine(QStringLiteral("y")));
        ScrollbackTail tail;
        sb.tailFrom(last + 100, 4096, tail);
        QVERIFY(!tail.resync);
        QCOMPARE(tail.fromLineId, last);
        QCOMPARE(tail.lines.size(), std::size_t(1));
        QCOMPARE(tail.lines.back().id, last);
    }

    // sinceId 已被头部淘汰：resync。
    {
        NovaTerm::ChunkedScrollback sb(2, 1024 * 1024, 2);  // 最多 2 行
        const LineId a = sb.append(textLine(QStringLiteral("a")));
        (void)sb.append(textLine(QStringLiteral("b")));
        (void)sb.append(textLine(QStringLiteral("c")));  // 淘汰 a
        (void)sb.append(textLine(QStringLiteral("d")));  // 淘汰 b
        ScrollbackTail tail;
        sb.tailFrom(a, 4096, tail);
        QVERIFY(tail.resync);
    }

    // 落后超过 maxLines：resync（交给全量重排，避免 GUI 线程深拷贝海量行）。
    {
        NovaTerm::ChunkedScrollback sb(100, 1024 * 1024, 4);
        const LineId first = sb.append(textLine(QStringLiteral("l0")));
        for (int i = 1; i < 10; ++i)
            sb.append(textLine(QStringLiteral("lN")));
        ScrollbackTail tail;
        sb.tailFrom(first, 3, tail);  // 跨度 10 > 3
        QVERIFY(tail.resync);
    }
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

    const std::vector<NovaTerm::DisplayLine> rows =
        NovaTerm::LineLayout::wrapLine(line, 2);
    QCOMPARE(rows.size(), std::size_t(3));
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
    request.query = "hello";
    request.generation = 1;
    request.resultBatchSize = 1;
    search.search(scrollback.snapshot(), request);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::SearchBatch>().completed, 3000);

    std::vector<NovaTerm::SearchMatch> matches;
    for (const QList<QVariant>& arguments : spy) {
        const auto batch = arguments.at(0).value<NovaTerm::SearchBatch>();
        matches.insert(matches.end(), batch.matches.begin(),
                       batch.matches.end());
    }
    QCOMPARE(matches.size(), std::size_t(2));
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
    oldRequest.query = "test";
    oldRequest.generation = 1;
    search.search(scrollback.snapshot(), oldRequest);

    NovaTerm::SearchRequest currentRequest;
    currentRequest.query = "missing";
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
    QCOMPARE(first.chunks().size(), std::size_t(1));
    QCOMPARE(scrollback.statistics().activeLines, qsizetype(0));
    QCOMPARE(scrollback.statistics().sealedChunks, qsizetype(1));

    scrollback.append(textLine(QStringLiteral("new tail")));
    const auto second = scrollback.snapshot();
    QCOMPARE(second.chunks().size(), std::size_t(2));
    QCOMPARE(first.chunks().front().chunk.get(),
             second.chunks().front().chunk.get());
    QCOMPARE(first.version(), quint64(1));
    QCOMPARE(second.version(), quint64(2));
}

// 软换行片段到达时，上一行可能已经随 active 块封存进不可变分块。此时
// appendContinuation 必须仍与它拼成同一条逻辑行 —— 实现把那一行搬进新的
// active 块，而不是复制整个分块（1024 行/块时代价约 4 MB）。
void ScrollbackTests::continuationAcrossSealedChunkKeepsOneLogicalLine()
{
    // chunkLines=2：追加两行即封存，使下一次续接必然跨越封存边界。
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 2);
    scrollback.append(textLine(QStringLiteral("aa")));
    NovaTerm::LogicalLine soft = textLine(QStringLiteral("bb"));
    soft.hardBreak = false;
    const NovaTerm::LineId softId = scrollback.append(std::move(soft));
    QCOMPARE(scrollback.lineCount(), qsizetype(2));
    QCOMPARE(scrollback.statistics().sealedChunks, qsizetype(1));

    // 续接片段落在封存边界之后，仍应并入 softId 那一行而非新增一行。
    NovaTerm::LogicalLine fragment = textLine(QStringLiteral("cc"));
    fragment.hardBreak = true;
    QCOMPARE(scrollback.appendContinuation(std::move(fragment)), softId);
    QCOMPARE(scrollback.lineCount(), qsizetype(2));

    const NovaTerm::LogicalLine* merged = scrollback.lineAt(1);
    QVERIFY(merged);
    QCOMPARE(merged->id, softId);
    QCOMPARE(merged->cells.size(), std::size_t(4));
    QCOMPARE(merged->cells[2].chars[0], uint32_t('c'));
    QVERIFY(merged->hardBreak);
    // 第一行不受影响，且 ID 仍严格单调（快照二分查找的前提）。
    QCOMPARE(scrollback.lineAt(0)->cells.size(), std::size_t(2));
    QVERIFY(scrollback.lineAt(0)->id < merged->id);

    // 快照必须完整包含两行，不能因为尾行被搬走而漏掉。
    const auto snapshot = scrollback.snapshot();
    QCOMPARE(snapshot.lineCount(), qsizetype(2));
    QCOMPARE(snapshot.lineById(softId)->cells.size(), std::size_t(4));
    QCOMPARE(snapshot.lastLineId(), softId);
}

// sb_popline 走的 takeNewestTail 同样会遇到"最新行已封存"，且它会缩短该行。
void ScrollbackTests::takeNewestTailAcrossSealedChunkPreservesHistory()
{
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 2);
    scrollback.append(textLine(QStringLiteral("keep")));
    const NovaTerm::LineId tailId =
        scrollback.append(textLine(QStringLiteral("abcdef")));
    QCOMPARE(scrollback.statistics().sealedChunks, qsizetype(1));

    NovaTerm::LogicalLine taken;
    QVERIFY(scrollback.takeNewestTail(2, taken));
    QCOMPARE(taken.id, tailId);
    QCOMPARE(taken.cells.size(), std::size_t(2));
    QCOMPARE(taken.cells[0].chars[0], uint32_t('e'));

    // 剩余部分留在历史里，且不再以硬换行结尾（尾段已回到活动屏幕）。
    QCOMPARE(scrollback.lineCount(), qsizetype(2));
    const NovaTerm::LogicalLine* remainder = scrollback.lineAt(1);
    QVERIFY(remainder);
    QCOMPARE(remainder->cells.size(), std::size_t(4));
    QVERIFY(!remainder->hardBreak);
    QCOMPARE(scrollback.lineAt(0)->cells.size(), std::size_t(4));

    // 取空整行时该行被移除，前一行仍完好。
    QVERIFY(scrollback.takeNewestTail(4, taken));
    QCOMPARE(taken.cells.size(), std::size_t(4));
    QCOMPARE(scrollback.lineCount(), qsizetype(1));
    QCOMPARE(scrollback.lineAt(0)->cells[0].chars[0], uint32_t('k'));
}

// 改写封存分块的尾行不得影响已发出的快照 —— 旧快照仍看到改写前的内容，
// 且分块本身仍被共享（没有整块复制）。
void ScrollbackTests::sealedChunkStaysSharedWhenItsTailIsRewritten()
{
    NovaTerm::ChunkedScrollback scrollback(100, 1024 * 1024, 2);
    scrollback.append(textLine(QStringLiteral("head")));
    NovaTerm::LogicalLine soft = textLine(QStringLiteral("xx"));
    soft.hardBreak = false;
    const NovaTerm::LineId softId = scrollback.append(std::move(soft));

    const auto before = scrollback.snapshot();
    QCOMPARE(before.lineCount(), qsizetype(2));
    QCOMPARE(before.lineById(softId)->cells.size(), std::size_t(2));

    NovaTerm::LogicalLine fragment = textLine(QStringLiteral("yy"));
    scrollback.appendContinuation(std::move(fragment));

    // 旧快照不可变：仍是 2 格。
    QCOMPARE(before.lineById(softId)->cells.size(), std::size_t(2));
    const auto after = scrollback.snapshot();
    QCOMPARE(after.lineById(softId)->cells.size(), std::size_t(4));
    QCOMPARE(after.lineCount(), qsizetype(2));

    // "head" 所在分块在两次快照中是同一个对象 —— 证明未整块复制。
    QCOMPARE(before.chunks().front().chunk.get(),
             after.chunks().front().chunk.get());
    QCOMPARE(before.lineAt(0)->id, after.lineAt(0)->id);
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
    current.query = "generation";
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
    request.query = QString::fromUtf8("😀中").toStdString();  // "😀中" UTF-8
    request.generation = 1;
    search.search(scrollback.snapshot(), request);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        spy.last().at(0).value<NovaTerm::SearchBatch>().completed, 3000);
    const auto matches = spy.first().at(0).value<NovaTerm::SearchBatch>().matches;
    QCOMPARE(matches.size(), std::size_t(1));
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
    unsafe.query = "(a+)+$";
    unsafe.regularExpression = true;
    unsafe.generation = 1;
    search.search(scrollback.snapshot(), unsafe);
    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 3000);
    QVERIFY(!spy.last().at(0).value<NovaTerm::SearchBatch>().error.empty());

    NovaTerm::SearchRequest limited;
    limited.query = "a";
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
        request.query = "missing";
        request.generation = 1;
        search.search(snapshot, request);
    }
    QVERIFY2(timer.elapsed() < 2000, "SearchEngine teardown exceeded 2 seconds");
}

QTEST_GUILESS_MAIN(ScrollbackTests)

#include "ScrollbackTests.moc"
