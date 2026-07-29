#include "core/terminal/BoundedByteQueue.h"
#include "core/terminal/ScrollbackBuffer.h"
#include "core/terminal/TerminalCore.h"

#include <QSignalSpy>
#include <QtTest>

#include <memory>
#include <vector>

class TerminalCoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void parsesUtf8AndAttributes();
    void parsesFragmentedUtf8();
    void reportsDamage();
    void resizesScreen();
    void snapshotsAreStableValues();
    void publishesTerminalTitle();
    void scrollbackKeepsNewestLines();
    void boundedByteQueuePreservesOrderAndBackpressure();
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
