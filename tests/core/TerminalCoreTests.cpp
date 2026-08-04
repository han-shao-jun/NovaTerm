#include "core/terminal/BoundedByteQueue.h"
#include "core/terminal/ScrollbackBuffer.h"
#include "core/terminal/TerminalCore.h"

#include <QSignalSpy>
#include <QtTest>

#include <memory>
#include <atomic>
#include <thread>
#include <vector>

class TerminalCoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void parsesUtf8AndAttributes();
    void parsesAnsiIndexedAndTrueColors();
    void parsesFragmentedUtf8();
    void reportsDamage();
    void ctrlCProducesInterruptCharacter();
    void resizesScreen();
    void resizePublishesFullDamageWithoutLiveScroll();
    void narrowerResizeReflowsExistingContent();
    void snapshotsAreStableValues();
    void modelPublicationsHaveMonotonicRevisions();
    void rendererSnapshotCopiesOnlyDirtyRows();
    void rendererSnapshotPublishesPerRowRevisions();
    void rendererSnapshotRowsRemainImmutableAcrossPublication();
    void publishesTerminalTitle();
    void scrollbackKeepsNewestLines();
    void softWrappedRowsBecomeOneLogicalHistoryLine();
    void rendererSnapshotUsesLogicalWrapAnchor();
    void liveRendererSnapshotDoesNotPublishHistoryTail();
    void fullScreenScrollPreservesContent();
    void reverseIndexScrollPreservesContent();
    void partialScrollRegionPreservesOutsideRows();
    void alternateScreenKeepsIndependentRowRing();
    void boundedByteQueuePreservesOrderAndBackpressure();
    void boundedByteQueueWakesBlockedProducer();
    void parserInputBackpressureDoesNotBlockCaller();
    void parserWorkerBatchesAndPublishes();
    void resizeAndShutdownUnderLoad();
};

void TerminalCoreTests::parsesUtf8AndAttributes()
{
    TerminalCore core(20, 4);

    core.writeInput(QByteArrayLiteral("\x1b[31mA"));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('A'));
    QCOMPARE(cell.foreground.type, NovaTerm::ColorType::Indexed);
    QCOMPARE(cell.foreground.index, uint8_t(1));

    core.writeInput(QString::fromUtf8(u8"中").toUtf8());
    QVERIFY(core.waitForIdle());

    QVERIFY(core.getCell(0, 1, cell));
    QCOMPARE(cell.chars[0], uint32_t(0x4E2D));
    QCOMPARE(int(cell.width), 2);
}

void TerminalCoreTests::reportsDamage()
{
    TerminalCore core(20, 4);
    QSignalSpy damageSpy(&core, &TerminalCore::damage);

    core.writeInput(QByteArrayLiteral("damage"));
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!damageSpy.isEmpty());
}

void TerminalCoreTests::ctrlCProducesInterruptCharacter()
{
    for (const QString& platformText : {QStringLiteral("c"),
                                        QString(QChar(0x03)), QString()}) {
        TerminalCore core(20, 4);
        QSignalSpy outputSpy(&core, &TerminalCore::outputData);
        QKeyEvent event(QEvent::KeyPress, Qt::Key_C,
                        Qt::ControlModifier, platformText);

        core.processKeyPress(&event);
        QVERIFY(core.waitForIdle());
        QTRY_VERIFY(!outputSpy.isEmpty());

        QByteArray output;
        for (const auto& arguments : outputSpy)
            output += arguments.at(0).toByteArray();
        QCOMPARE(output, QByteArray(1, '\x03'));
    }
}

void TerminalCoreTests::parsesFragmentedUtf8()
{
    TerminalCore core(20, 4);
    const QByteArray text = QString::fromUtf8(u8"中").toUtf8();

    core.writeInput(text.left(1));
    core.writeInput(text.mid(1, 1));
    core.writeInput(text.mid(2));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t(0x4E2D));
    QCOMPARE(cell.width, uint8_t(2));
}

void TerminalCoreTests::resizesScreen()
{
    TerminalCore core(80, 24);

    core.resize(132, 40);
    QVERIFY(core.waitForIdle());

    QCOMPARE(core.columns(), 132);
    QCOMPARE(core.rows(), 40);
}

void TerminalCoreTests::resizePublishesFullDamageWithoutLiveScroll()
{
    TerminalCore core(24, 5);
    core.writeInput(QByteArrayLiteral(
        "line-1-abcdefghijklmnop\r\n"
        "line-2-abcdefghijklmnop\r\n"
        "line-3-abcdefghijklmnop\r\n"
        "line-4-abcdefghijklmnop"));
    QVERIFY(core.waitForIdle());
    QCoreApplication::processEvents();

    QSignalSpy damageSpy(&core, &TerminalCore::damage);
    QSignalSpy scrollSpy(&core, &TerminalCore::screenScrolled);
    core.resize(10, 5);
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!damageSpy.isEmpty());

    bool hasFullResizeDamage = false;
    for (const QList<QVariant>& arguments : damageSpy) {
        const auto region =
            qvariant_cast<NovaTerm::DirtyRegion>(arguments.at(0));
        if (region.startRow == 0 && region.endRow == 5
            && region.startColumn == 0 && region.endColumn == 10) {
            hasFullResizeDamage = true;
            break;
        }
    }
    QVERIFY(hasFullResizeDamage);
    QCOMPARE(scrollSpy.count(), 0);
}

void TerminalCoreTests::parsesAnsiIndexedAndTrueColors()
{
    TerminalCore core(20, 4);
    core.writeInput(QByteArrayLiteral(
        "\x1b[31mR\x1b[38;5;214mI\x1b[38;2;12;34;56mT"));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.foreground.type, NovaTerm::ColorType::Indexed);
    QCOMPARE(cell.foreground.index, uint8_t(1));

    QVERIFY(core.getCell(0, 1, cell));
    QCOMPARE(cell.foreground.type, NovaTerm::ColorType::Indexed);
    QCOMPARE(cell.foreground.index, uint8_t(214));

    QVERIFY(core.getCell(0, 2, cell));
    QCOMPARE(cell.foreground.type, NovaTerm::ColorType::Rgb);
    QCOMPARE(cell.foreground.red, uint8_t(12));
    QCOMPARE(cell.foreground.green, uint8_t(34));
    QCOMPARE(cell.foreground.blue, uint8_t(56));

    core.writeInput(QByteArrayLiteral("\x1b[0mD"));
    QVERIFY(core.waitForIdle());
    QVERIFY(core.getCell(0, 3, cell));
    QCOMPARE(cell.foreground.type, NovaTerm::ColorType::Default);
}

void TerminalCoreTests::narrowerResizeReflowsExistingContent()
{
    TerminalCore core(20, 6);
    core.writeInput(QByteArrayLiteral("123456789012345"));
    QVERIFY(core.waitForIdle());

    core.resize(8, 6);
    QVERIFY(core.waitForIdle());

    const auto snapshot = core.snapshot();
    QStringList populatedRows;
    for (int row = 0; row < snapshot.rows; ++row) {
        QString text;
        for (int column = 0; column < snapshot.columns; ++column) {
            const auto* cell = snapshot.cellAt(row, column);
            if (!cell || cell->chars[0] == 0)
                break;
            text.append(QChar(cell->chars[0]));
        }
        if (!text.isEmpty())
            populatedRows.push_back(text);
    }

    QCOMPARE(populatedRows,
             QStringList({QStringLiteral("12345678"),
                          QStringLiteral("9012345")}));
}

void TerminalCoreTests::snapshotsAreStableValues()
{
    TerminalCore core(20, 4);
    core.writeInput(QByteArrayLiteral("A"));
    QVERIFY(core.waitForIdle());
    const NovaTerm::TerminalSnapshot before = core.snapshot();

    core.writeInput(QByteArrayLiteral("\rB"));
    QVERIFY(core.waitForIdle());
    const NovaTerm::TerminalSnapshot after = core.snapshot();

    QVERIFY(before.cellAt(0, 0));
    QVERIFY(after.cellAt(0, 0));
    QCOMPARE(before.cellAt(0, 0)->chars[0], uint32_t('A'));
    QCOMPARE(after.cellAt(0, 0)->chars[0], uint32_t('B'));
    QVERIFY(after.revision > before.revision);
}

void TerminalCoreTests::modelPublicationsHaveMonotonicRevisions()
{
    TerminalCore core(20, 4);
    QSignalSpy damageSpy(&core, &TerminalCore::damage);

    core.writeInput(QByteArrayLiteral("A"));
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!damageSpy.isEmpty());
    const quint64 firstSignalRevision =
        damageSpy.last().at(1).toULongLong();
    const quint64 firstSnapshotRevision = core.snapshot().revision;
    QCOMPARE(firstSignalRevision, firstSnapshotRevision);

    damageSpy.clear();
    core.writeInput(QByteArrayLiteral("B"));
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!damageSpy.isEmpty());
    const quint64 secondSignalRevision =
        damageSpy.last().at(1).toULongLong();
    QVERIFY(secondSignalRevision > firstSignalRevision);
    QCOMPARE(core.modelRevision(), secondSignalRevision);
}

void TerminalCoreTests::rendererSnapshotCopiesOnlyDirtyRows()
{
    TerminalCore core(20, 4);
    core.writeInput(QByteArrayLiteral("\x1b[2;1HX"));
    QVERIFY(core.waitForIdle());

    QVector<bool> dirtyRows(4, false);
    dirtyRows[1] = true;
    const NovaTerm::RendererSnapshot snapshot =
        core.rendererSnapshot(dirtyRows, 0);

    QCOMPARE(snapshot.columns, 20);
    QCOMPARE(snapshot.rows, 4);
    QVERIFY(snapshot.cellAt(0, 0) == nullptr);
    QVERIFY(snapshot.cellAt(1, 0));
    QCOMPARE(snapshot.cellAt(1, 0)->chars[0], uint32_t('X'));
    QVERIFY(snapshot.cellAt(2, 0) == nullptr);
}

void TerminalCoreTests::rendererSnapshotPublishesPerRowRevisions()
{
    TerminalCore core(20, 4);
    core.writeInput(QByteArrayLiteral("A"));
    QVERIFY(core.waitForIdle());
    const quint64 firstRevision = core.modelRevision();

    core.writeInput(QByteArrayLiteral("\x1b[3;1HZ"));
    QVERIFY(core.waitForIdle());
    QVector<bool> dirtyRows(4, true);
    const auto snapshot = core.rendererSnapshot(dirtyRows, 0);

    QCOMPARE(snapshot.visibleRowRevisions.size(), 4);
    QVERIFY(snapshot.revision > firstRevision);
    QCOMPARE(snapshot.visibleRowRevisions[2], snapshot.revision);
    QVERIFY(snapshot.visibleRowRevisions[1] < snapshot.revision);
}

void TerminalCoreTests::rendererSnapshotRowsRemainImmutableAcrossPublication()
{
    TerminalCore core(20, 4);
    core.writeInput(QByteArrayLiteral("A"));
    QVERIFY(core.waitForIdle());
    QVector<bool> dirtyRows(4, false);
    dirtyRows[0] = true;
    const auto before = core.rendererSnapshot(dirtyRows, 0);
    QVERIFY(before.visibleRows[0]);
    const auto retainedRow = before.visibleRows[0];
    const quint64 beforeIdentity = before.visibleRowIdentities[0];

    core.writeInput(QByteArrayLiteral("\rB"));
    QVERIFY(core.waitForIdle());
    const auto after = core.rendererSnapshot(dirtyRows, 0);

    QCOMPARE(retainedRow->at(0).chars[0], uint32_t('A'));
    QCOMPARE(before.cellAt(0, 0)->chars[0], uint32_t('A'));
    QCOMPARE(after.cellAt(0, 0)->chars[0], uint32_t('B'));
    QVERIFY(after.visibleRows[0] != retainedRow);
    QVERIFY(after.visibleRowIdentities[0] != beforeIdentity);
    QVERIFY(after.revision > before.revision);
}

void TerminalCoreTests::publishesTerminalTitle()
{
    TerminalCore core(20, 4);
    QSignalSpy titleSpy(&core, &TerminalCore::titleChanged);

    core.writeInput(QByteArrayLiteral("\x1b]2;P1 adapter title\x07"));
    QVERIFY(core.waitForIdle());

    QCOMPARE(core.title(), QStringLiteral("P1 adapter title"));
    QTRY_COMPARE(titleSpy.size(), 1);
}

void TerminalCoreTests::scrollbackKeepsNewestLines()
{
    ScrollbackBuffer buffer(3);
    std::vector<NovaTerm::Cell> cells(2);

    for (uint32_t value = 'A'; value <= 'D'; ++value) {
        cells.assign(cells.size(), NovaTerm::Cell{});
        cells[0].chars[0] = value;
        cells[0].width = 1;
        buffer.pushLine(cells.data(), int(cells.size()));
    }

    QCOMPARE(buffer.lineCount(), 3);
    QCOMPARE(buffer.columns(), 2);
    QCOMPARE(buffer.lineAt(0)[0].chars[0], uint32_t('B'));
    QCOMPARE(buffer.lineAt(1)[0].chars[0], uint32_t('C'));
    QCOMPARE(buffer.lineAt(2)[0].chars[0], uint32_t('D'));
}

void TerminalCoreTests::softWrappedRowsBecomeOneLogicalHistoryLine()
{
    TerminalCore core(4, 2);
    core.setScrollbackLimit(100);
    QVERIFY(core.waitForIdle());
    core.writeInput(QByteArrayLiteral("abcdefghijklmnopqr"));
    QVERIFY(core.waitForIdle());

    const auto history = core.scrollbackSnapshot();
    QCOMPARE(history.lineCount(), qsizetype(1));
    const auto* line = history.lineAt(0);
    QVERIFY(line);
    QCOMPARE(line->cells.size(), qsizetype(12));
    QString text;
    for (const auto& cell : line->cells)
        text += QChar(cell.chars[0]);
    QCOMPARE(text, QStringLiteral("abcdefghijkl"));
    QVERIFY(!line->hardBreak);
}

void TerminalCoreTests::rendererSnapshotUsesLogicalWrapAnchor()
{
    TerminalCore core(4, 2);
    core.writeInput(QByteArrayLiteral("abcdefghijklmnopqr"));
    QVERIFY(core.waitForIdle());
    const auto history = core.scrollbackSnapshot();
    QVERIFY(!history.empty());
    QVector<bool> dirty(2, true);
    const auto rendered = core.rendererSnapshot(
        dirty, 1, history.firstLineId(), 2);
    QVERIFY(rendered.cellAt(0, 0));
    QCOMPARE(rendered.cellAt(0, 0)->chars[0], uint32_t('i'));
    QVERIFY(rendered.cellAt(0, 3));
    QCOMPARE(rendered.cellAt(0, 3)->chars[0], uint32_t('l'));
}

void TerminalCoreTests::liveRendererSnapshotDoesNotPublishHistoryTail()
{
    TerminalCore core(8, 2);
    core.writeInput(QByteArrayLiteral("first\r\nsecond\r\nthird"));
    QVERIFY(core.waitForIdle());
    const auto before = core.scrollbackStatistics();
    QVERIFY(before.activeLines > 0);

    QVector<bool> dirty(2, true);
    const auto rendered = core.rendererSnapshot(dirty, 0);
    QVERIFY(rendered.cellAt(0, 0));
    const auto after = core.scrollbackStatistics();
    QCOMPARE(after.activeLines, before.activeLines);
    QCOMPARE(after.sealedChunks, before.sealedChunks);
}

void TerminalCoreTests::fullScreenScrollPreservesContent()
{
    TerminalCore core(8, 3);
    core.setScrollbackLimit(10);
    QVERIFY(core.waitForIdle());

    core.writeInput(QByteArrayLiteral(
        "line1\r\nline2\r\nline3\r\nline4"));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 4, cell));
    QCOMPARE(cell.chars[0], uint32_t('2'));
    QVERIFY(core.getCell(1, 4, cell));
    QCOMPARE(cell.chars[0], uint32_t('3'));
    QVERIFY(core.getCell(2, 4, cell));
    QCOMPARE(cell.chars[0], uint32_t('4'));

    QCOMPARE(core.scrollbackLineCount(), 1);
    QVERIFY(core.getScrollbackCell(0, 4, cell));
    QCOMPARE(cell.chars[0], uint32_t('1'));

    // Resize forces libvterm to normalize its row ring before reallocation.
    core.resize(10, 4);
    QVERIFY(core.waitForIdle());
    QCOMPARE(core.columns(), 10);
    QCOMPARE(core.rows(), 4);
}

void TerminalCoreTests::reverseIndexScrollPreservesContent()
{
    TerminalCore core(8, 3);
    core.writeInput(QByteArrayLiteral(
        "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[1;1H\x1bM"));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t(0));
    QVERIFY(core.getCell(1, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('A'));
    QVERIFY(core.getCell(2, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('B'));
}

void TerminalCoreTests::partialScrollRegionPreservesOutsideRows()
{
    TerminalCore core(8, 4);
    // First create a non-zero full-screen row offset, then overwrite the
    // visible rows before exercising the conservative partial-scroll path.
    core.writeInput(QByteArrayLiteral(
        "1\r\n2\r\n3\r\n4\r\n5"
        "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD"
        "\x1b[2;4r\x1b[4;1H\n"));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('A'));
    QVERIFY(core.getCell(1, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('C'));
    QVERIFY(core.getCell(2, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('D'));
    QVERIFY(core.getCell(3, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t(0));
}

void TerminalCoreTests::alternateScreenKeepsIndependentRowRing()
{
    TerminalCore core(8, 3);
    core.writeInput(QByteArrayLiteral(
        "p1\r\np2\r\np3\r\np4"
        "\x1b[?1049h"
        "a1\r\na2\r\na3\r\na4"
        "\x1b[?1049l"));
    QVERIFY(core.waitForIdle());

    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('p'));
    QVERIFY(core.getCell(0, 1, cell));
    QCOMPARE(cell.chars[0], uint32_t('2'));
    QVERIFY(core.getCell(2, 1, cell));
    QCOMPARE(cell.chars[0], uint32_t('4'));
}

void TerminalCoreTests::boundedByteQueuePreservesOrderAndBackpressure()
{
    NovaTerm::BoundedByteQueue queue(8);

    QVERIFY(queue.enqueue(QByteArrayLiteral("abcdef"), 0));
    QCOMPARE(queue.take(4, 0), QByteArrayLiteral("abcd"));
    QVERIFY(queue.enqueue(QByteArrayLiteral("WXYZ"), 0));
    QCOMPARE(queue.take(8, 0), QByteArrayLiteral("efWXYZ"));

    QVERIFY(queue.enqueue(QByteArrayLiteral("12345678"), 0));
    QVERIFY(!queue.enqueue(QByteArrayLiteral("x"), 0));
    const auto statistics = queue.statistics();
    QCOMPARE(statistics.capacity, qsizetype(8));
    QCOMPARE(statistics.highWatermark, qsizetype(8));
    QVERIFY(statistics.producerWaits >= 1);
}

void TerminalCoreTests::boundedByteQueueWakesBlockedProducer()
{
    NovaTerm::BoundedByteQueue queue(8);
    QVERIFY(queue.enqueue(QByteArrayLiteral("12345678"), 0));

    std::atomic<bool> completed{false};
    std::thread producer([&]() {
        const bool accepted = queue.enqueue(QByteArrayLiteral("x"));
        completed.store(accepted, std::memory_order_release);
    });

    QTRY_VERIFY_WITH_TIMEOUT(queue.statistics().producerWaits > 0, 1000);
    QVERIFY(!completed.load(std::memory_order_acquire));
    QCOMPARE(queue.take(1, 0), QByteArrayLiteral("1"));
    producer.join();
    QVERIFY(completed.load(std::memory_order_acquire));
    QCOMPARE(queue.take(8, 0), QByteArrayLiteral("2345678x"));
}

void TerminalCoreTests::parserInputBackpressureDoesNotBlockCaller()
{
    TerminalCore core(80, 24);
    QSignalSpy backpressureSpy(
        &core, &TerminalCore::inputBackpressureChanged);

    QElapsedTimer timer;
    timer.start();
    const QByteArray input(64 * 1024 * 1024, 'x');
    TerminalCore::InputWriteResult result = core.writeInput(input);
    QVERIFY(!result.fullyAccepted());
    QVERIFY(result.backpressured);
    QVERIFY(result.acceptedBytes > 0);
    QVERIFY(result.acceptedBytes < input.size());
    QVERIFY2(timer.elapsed() < 1000,
             "writeInput blocked instead of reporting bounded overload");

    qsizetype offset = result.acceptedBytes;
    while (offset < input.size()) {
        result = core.writeInput(QByteArrayView(input).sliced(offset));
        offset += result.acceptedBytes;
        if (!result.fullyAccepted())
            QTest::qWait(1);
    }

    QTRY_VERIFY_WITH_TIMEOUT(backpressureSpy.count() >= 1, 1000);
    QCOMPARE(backpressureSpy.first().at(0).toBool(), true);
    QVERIFY(core.waitForIdle(10'000));
    const auto statistics = core.queueStatistics();
    QCOMPARE(statistics.totalEnqueued, uint64_t(input.size()));
    QCOMPARE(statistics.totalDequeued, uint64_t(input.size()));
    QTRY_VERIFY_WITH_TIMEOUT(
        backpressureSpy.last().at(0).toBool() == false, 1000);
}

void TerminalCoreTests::parserWorkerBatchesAndPublishes()
{
    TerminalCore core(80, 24);
    QByteArray input;
    input.reserve(256 * 1024);
    while (input.size() < 256 * 1024)
        input += QByteArrayLiteral("P2 asynchronous parser line\r\n");

    for (qsizetype offset = 0; offset < input.size(); offset += 4096)
        core.writeInput(input.mid(offset, 4096));

    QVERIFY(core.waitForIdle(10'000));
    const auto statistics = core.queueStatistics();
    QCOMPARE(statistics.totalEnqueued, statistics.totalDequeued);
    QVERIFY(statistics.totalEnqueued >= uint64_t(input.size()));
    QVERIFY(core.scrollbackLineCount() > 0);
}

void TerminalCoreTests::resizeAndShutdownUnderLoad()
{
    for (int iteration = 0; iteration < 10; ++iteration) {
        auto core = std::make_unique<TerminalCore>(80, 24);
        core->writeInput(QByteArray(128 * 1024, 'x'));
        core->resize(100 + iteration, 30 + iteration);
        QVERIFY(core->waitForIdle(10'000));
        QCOMPARE(core->columns(), 100 + iteration);
        QCOMPARE(core->rows(), 30 + iteration);
    }
}

QTEST_GUILESS_MAIN(TerminalCoreTests)

#include "TerminalCoreTests.moc"
