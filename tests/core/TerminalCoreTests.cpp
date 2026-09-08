#include "core/terminal/BoundedByteQueue.h"
#include "core/terminal/KeyMapper.h"
#include "core/terminal/ScrollbackBuffer.h"
#include "core/terminal/TerminalCore.h"
#include "core/terminal/VTAdapter.h"

#include <QSignalSpy>
#include <QtTest>

#include <memory>
#include <atomic>
#include <cstring>
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
    void largeBracketedPasteIsBatchedAndOrdered();
    void pasteNormalizesNewlinesToCarriageReturn();
    void resizesScreen();
    void resizePublishesFullDamageWithoutLiveScroll();
    void narrowerResizeReflowsExistingContent();
    void snapshotsAreStableValues();
    void modelPublicationsHaveMonotonicRevisions();
    void rendererSnapshotCopiesOnlyDirtyRows();
    void rendererSnapshotPublishesPerRowRevisions();
    void rendererSnapshotRowsRemainImmutableAcrossPublication();
    void publishesTerminalTitle();
    void cursorPropertiesPublishWithoutFollowingMovement();
    void scrollbackKeepsNewestLines();
    void softWrappedRowsBecomeOneLogicalHistoryLine();
    void softWrapKeepsTrailingSpaces();
    void rowContinuationTracksAutoWrap();
    void popLineReturnsNewestRowWithoutLoss();
    void wideCharMarksContinuationCell();
    void rendererSnapshotUsesLogicalWrapAnchor();
    void rendererSnapshotFallsBackWhenAnchorEvicted();
    void liveRendererSnapshotDoesNotPublishHistoryTail();
    void fullScreenScrollPreservesContent();
    void batchedScreenEditsMatchIncrementalInput();
    void reverseIndexScrollPreservesContent();
    void partialScrollRegionPreservesOutsideRows();
    void alternateScreenKeepsIndependentRowRing();
    void boundedByteQueuePreservesOrderAndBackpressure();
    void boundedByteQueueWakesBlockedProducer();
    void parserInputBackpressureDoesNotBlockCaller();
    void parserWorkerBatchesAndPublishes();
    void resizeAndShutdownUnderLoad();
    void keyMapperMapsSpecialKeysAndModifiers();
    void keyMapperMapsControlCharacters();
    void altGrProducesPrintableCharacterNotControlCode();
    void shiftSpaceSendsPlainSpace();
    void altLetterSendsMetaEscapePrefix();
};

namespace {

// 把 QByteArray 作为 ByteView 喂给 VTAdapter（core 接口已去 Qt）。
void writeBytes(NovaTerm::VTAdapter& adapter, const QByteArray& data)
{
    adapter.writeInput(NovaTerm::ByteView(data.constData(), data.size()));
}

} // namespace

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

    std::vector<bool> dirtyRows(4, false);
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
    std::vector<bool> dirtyRows(4, true);
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
    std::vector<bool> dirtyRows(4, false);
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

void TerminalCoreTests::largeBracketedPasteIsBatchedAndOrdered()
{
    TerminalCore core(80, 24);
    core.writeInput(QByteArrayLiteral("\x1b[?2004h"));
    QVERIFY(core.waitForIdle());

    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    const QString text(1024 * 1024, QLatin1Char('P'));
    core.pasteText(text);
    QVERIFY(core.waitForIdle(10000));
    QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 5000);

    QByteArray output;
    for (const QList<QVariant>& arguments : outputSpy)
        output.append(arguments.at(0).toByteArray());
    QCOMPARE(output, QByteArrayLiteral("\x1b[200~") + text.toUtf8()
                         + QByteArrayLiteral("\x1b[201~"));
    QVERIFY2(outputSpy.size() <= 20,
             "large paste generated character-sized output signals");
}

void TerminalCoreTests::pasteNormalizesNewlinesToCarriageReturn()
{
    TerminalCore core(80, 24);
    QVERIFY(core.waitForIdle());

    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    // 混合 CRLF（Windows 剪贴板）、孤立 LF（Unix）与孤立 CR。
    core.pasteText(QStringLiteral("a\r\nb\nc\rd"));
    QVERIFY(core.waitForIdle(10'000));
    QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 5000);

    QByteArray output;
    for (const QList<QVariant>& arguments : outputSpy)
        output.append(arguments.at(0).toByteArray());
    // 所有换行统一为单个 \r；不得残留 \n 或 \r\n（否则 raw 模式 PTY
    // 会把 \r\n 当作两次回车，导致粘贴行重复）。
    QCOMPARE(output, QByteArrayLiteral("a\rb\rc\rd"));
    QVERIFY(!output.contains('\n'));
    QVERIFY(!output.contains(QByteArrayLiteral("\r\n")));
}

void TerminalCoreTests::cursorPropertiesPublishWithoutFollowingMovement()
{
    TerminalCore core(20, 4);
    QSignalSpy cursorSpy(&core, &TerminalCore::cursorMoved);

    // Local line editors may temporarily select a steady cursor, move it,
    // then restore blinking without another cursor movement. The final
    // property callback must therefore publish independently of movecursor.
    core.writeInput(QByteArrayLiteral(
        "\x1b[6 q"       // steady bar
        "\x1b[2C"        // move right while the cursor is steady
        "\x1b[5 q"));    // blinking bar, with no following movement
    QVERIFY(core.waitForIdle());

    QTRY_VERIFY(!cursorSpy.isEmpty());
    const NovaTerm::CursorState cursor = core.cursorState();
    QCOMPARE(cursor.position.col, 2);
    QCOMPARE(cursor.shape, NovaTerm::CursorShape::BarLeft);
    QVERIFY(cursor.visible);
    QVERIFY(cursor.blink);

    const int publications = cursorSpy.size();
    core.writeInput(QByteArrayLiteral("\x1b[?25l"));
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(cursorSpy.size() > publications);
    QVERIFY(!core.cursorState().visible);
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

// 软换行行的行尾空格是有效内容 —— 下一行的文本紧接其后。旧实现无条件裁剪
// 行尾空 Cell，逻辑行会短掉那几格，按新列宽重排后后续文本整体左移。
void TerminalCoreTests::softWrapKeepsTrailingSpaces()
{
    TerminalCore core(4, 2);
    core.setScrollbackLimit(100);
    QVERIFY(core.waitForIdle());
    // 4 列下依次产生 "ab  "、"cdef"、"ghij"、"kl"；前两行滚入历史并合成
    // 一条逻辑行。第一行填满 4 列因而是软换行，其行尾两个空格必须保留。
    core.writeInput(QByteArrayLiteral("ab  cdefghijkl"));
    QVERIFY(core.waitForIdle());

    const auto history = core.scrollbackSnapshot();
    QCOMPARE(history.lineCount(), qsizetype(1));
    const auto* line = history.lineAt(0);
    QVERIFY(line);
    QCOMPARE(line->cells.size(), qsizetype(8));
    QString text;
    for (const auto& cell : line->cells) {
        text += cell.chars[0] ? QChar(cell.chars[0]) : QLatin1Char(' ');
    }
    QCOMPARE(text, QStringLiteral("ab  cdef"));
    QVERIFY(!line->hardBreak);
}

// 活动屏幕的每行软换行标志从 libvterm 的 VTermLineInfo::continuation 同步。
void TerminalCoreTests::rowContinuationTracksAutoWrap()
{
    TerminalCore core(4, 3);
    QVERIFY(core.waitForIdle());
    // 6 个字符在 4 列下自动换行：第 1 行是第 0 行的延续。
    core.writeInput(QByteArrayLiteral("abcdef"));
    QVERIFY(core.waitForIdle());
    QVERIFY(!core.rowContinuation(0));
    QVERIFY(core.rowContinuation(1));
    QVERIFY(!core.rowContinuation(2));
    // 越界不应崩，返回 false。
    QVERIFY(!core.rowContinuation(-1));
    QVERIFY(!core.rowContinuation(100));

    // 对照：真实换行不产生延续标志。
    TerminalCore hard(4, 3);
    QVERIFY(hard.waitForIdle());
    hard.writeInput(QByteArrayLiteral("ab\r\ncd"));
    QVERIFY(hard.waitForIdle());
    QVERIFY(!hard.rowContinuation(0));
    QVERIFY(!hard.rowContinuation(1));
}

// libvterm 在屏幕变高时用 sb_popline 反向取回紧邻屏幕顶部的那一行 —— 是
// 最新的历史行。旧实现取最旧一行，且把整条逻辑行弹出后只回填前 cols 格，
// 其余 Cell 被永久丢弃。
void TerminalCoreTests::popLineReturnsNewestRowWithoutLoss()
{
    ScrollbackBuffer buffer;
    buffer.setMaxLines(100);

    const auto pushRow = [&buffer](const QString& text, bool continuation,
                                   bool hardBreak) {
        std::vector<NovaTerm::Cell>& row =
            buffer.beginPushLine(4, int(text.size()));
        for (int i = 0; i < text.size(); ++i) {
            row[i].chars[0] = text[i].unicode();
            row[i].width = 1;
        }
        buffer.commitPushLine(continuation, hardBreak);
    };

    // 一条由 3 个 4 列屏幕行软换行而成的逻辑行，共 12 格。
    pushRow(QStringLiteral("aaaa"), false, false);
    pushRow(QStringLiteral("bbbb"), true, false);
    pushRow(QStringLiteral("cccc"), true, true);
    QCOMPARE(buffer.lineCount(), 1);
    QCOMPARE(buffer.lineVectorAt(0)->size(), qsizetype(12));

    NovaTerm::Cell popped[4];
    QVERIFY(buffer.popLine(popped, 4));
    // 取回的是最新的屏幕行 "cccc"，不是最旧的 "aaaa"。
    for (const NovaTerm::Cell& cell : popped)
        QCOMPARE(QChar(cell.chars[0]), QLatin1Char('c'));
    // 其余 8 格留在历史中，未随整条逻辑行被丢弃。
    QCOMPARE(buffer.lineCount(), 1);
    QCOMPARE(buffer.lineVectorAt(0)->size(), qsizetype(8));

    // 继续取回：应依次得到 "bbbb"、"aaaa"，然后耗尽。
    QVERIFY(buffer.popLine(popped, 4));
    for (const NovaTerm::Cell& cell : popped)
        QCOMPARE(QChar(cell.chars[0]), QLatin1Char('b'));
    QVERIFY(buffer.popLine(popped, 4));
    for (const NovaTerm::Cell& cell : popped)
        QCOMPARE(QChar(cell.chars[0]), QLatin1Char('a'));
    QCOMPARE(buffer.lineCount(), 0);
    QVERIFY(!buffer.popLine(popped, 4));
}

// 双宽字符占两个网格位置，后一格必须携带 WideCharContinuation 哨兵 ——
// Renderer 据此避免为它重复生成字形，LineLayout 据此保证折行不把宽字符
// 劈开，SearchEngine 据此跳过它。
//
// libvterm 内部用 chars[0] = (uint32_t)-1 标记该格（screen.c:198），而
// vterm_screen_get_cell 逐字复制 chars（screen.c:1040-1044），数值恰好等于
// NovaTerm 的 WideCharContinuation，因此 populateCell() 的按零终止符复制
// 会自然把哨兵带过来 —— 不需要额外写入点。本测试锁定这条链路。
void TerminalCoreTests::wideCharMarksContinuationCell()
{
    TerminalCore core(8, 2);
    QVERIFY(core.waitForIdle());
    core.writeInput(QString::fromUtf8(u8"中A").toUtf8());
    QVERIFY(core.waitForIdle());

    // 第 0 列是宽字符本体，width=2。
    NovaTerm::Cell lead;
    QVERIFY(core.getCell(0, 0, lead));
    QCOMPARE(lead.chars[0], uint32_t(0x4E2D));
    QCOMPARE(int(lead.width), 2);
    QVERIFY(!lead.isWideContinuation());

    // 第 1 列是它的视觉延续格。
    NovaTerm::Cell continuation;
    QVERIFY(core.getCell(0, 1, continuation));
    QVERIFY2(continuation.isWideContinuation(),
             "the grid cell behind a double-width glyph must carry "
             "WideCharContinuation");

    // 紧随其后的窄字符落在第 2 列，不是延续格。
    NovaTerm::Cell next;
    QVERIFY(core.getCell(0, 2, next));
    QCOMPARE(next.chars[0], uint32_t('A'));
    QCOMPARE(int(next.width), 1);
    QVERIFY(!next.isWideContinuation());
}

void TerminalCoreTests::rendererSnapshotUsesLogicalWrapAnchor()
{
    TerminalCore core(4, 2);
    core.writeInput(QByteArrayLiteral("abcdefghijklmnopqr"));
    QVERIFY(core.waitForIdle());
    const auto history = core.scrollbackSnapshot();
    QVERIFY(!history.empty());
    std::vector<bool> dirty(2, true);
    const auto rendered = core.rendererSnapshot(
        dirty, 1, history.firstLineId(), 2);
    QVERIFY(rendered.cellAt(0, 0));
    QCOMPARE(rendered.cellAt(0, 0)->chars[0], uint32_t('i'));
    QVERIFY(rendered.cellAt(0, 3));
    QCOMPARE(rendered.cellAt(0, 3)->chars[0], uint32_t('l'));
}

// 回看期间锚点行被淘汰出历史时，回看区不能整片渲染成空白。传入一个早已
// 被淘汰的 anchorLine，快照仍应从当前历史末尾回退填充可见行。
void TerminalCoreTests::rendererSnapshotFallsBackWhenAnchorEvicted()
{
    TerminalCore core(4, 2);
    core.setScrollbackLimit(8);  // 小上限，逼早期行被淘汰
    QVERIFY(core.waitForIdle());
    // 写入远多于上限的行，使最早的历史行（含 lineId 1）被淘汰。
    for (int i = 0; i < 40; ++i)
        core.writeInput(QByteArrayLiteral("row\r\n"));
    QVERIFY(core.waitForIdle());

    const auto history = core.scrollbackSnapshot();
    QVERIFY(!history.empty());
    // lineId 1 是最早的行，此时应已被淘汰，lineById 命不中。
    QVERIFY(history.firstLineId() > 1);

    // 传入早已淘汰的 anchorLine=1，回看 2 行。修复前 historyViewport 为空，
    // 回看行全默认（黑屏）；修复后回退到历史末尾，行内应有真实内容。
    std::vector<bool> dirty(2, true);
    const auto rendered = core.rendererSnapshot(dirty, 2, /*anchorLine=*/1, 0);
    const NovaTerm::Cell* cell = rendered.cellAt(0, 0);
    QVERIFY(cell);
    QCOMPARE(cell->chars[0], uint32_t('r'));  // "row" 的首字符，而非空白
}

void TerminalCoreTests::liveRendererSnapshotDoesNotPublishHistoryTail()
{
    TerminalCore core(8, 2);
    core.writeInput(QByteArrayLiteral("first\r\nsecond\r\nthird"));
    QVERIFY(core.waitForIdle());
    const auto before = core.scrollbackStatistics();
    QVERIFY(before.activeLines > 0);

    std::vector<bool> dirty(2, true);
    const auto rendered = core.rendererSnapshot(dirty, 0);
    QVERIFY(rendered.cellAt(0, 0));
    const auto after = core.scrollbackStatistics();
    QCOMPARE(after.activeLines, before.activeLines);
    QCOMPARE(after.sealedChunks, before.sealedChunks);
}

void TerminalCoreTests::batchedScreenEditsMatchIncrementalInput()
{
    constexpr int columns = 40;
    constexpr int rows = 12;
    NovaTerm::ScreenBuffer batched(columns, rows);
    NovaTerm::ScreenBuffer incremental(columns, rows);
    ScrollbackBuffer batchedHistory;
    ScrollbackBuffer incrementalHistory;
    NovaTerm::VTAdapter batchAdapter(columns, rows, batched, batchedHistory, {});
    NovaTerm::VTAdapter incrementalAdapter(
        columns, rows, incremental, incrementalHistory, {});
    const QByteArray initial = "\x1b[2J\x1b[H";
    writeBytes(batchAdapter, initial);
    batchAdapter.flushDamage();
    writeBytes(incrementalAdapter, initial);
    incrementalAdapter.flushDamage();

    // 同一组定位、擦除和滚动操作，改变分批边界不应改变最终屏幕。
    quint32 seed = 42;
    const auto next = [&seed]() {
        seed = seed * 1664525U + 1013904223U;
        return seed;
    };
    for (int batch = 0; batch < 200; ++batch) {
        QByteArray input;
        for (int operation = 0; operation < 20; ++operation) {
            switch (next() % 7) {
            case 0: input += "\r\n"; break;
            case 1: input += "\x1b[S"; break;
            case 2: input += "\x1b[T"; break;
            case 3: input += "\x1b[K"; break;
            case 4: input += "\x1b[P"; break;
            case 5:
                input += "\x1b[" + QByteArray::number(next() % rows + 1)
                    + ";" + QByteArray::number(next() % columns + 1) + "H";
                break;
            default: input += QByteArray(5, char('A' + next() % 26)); break;
            }
        }
        writeBytes(batchAdapter, input);
        batchAdapter.flushDamage();
        for (char byte : input) {
            writeBytes(incrementalAdapter, QByteArray(1, byte));
            incrementalAdapter.flushDamage();
        }
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < columns; ++col) {
                const QByteArray context = "batch=" + QByteArray::number(batch)
                    + " row=" + QByteArray::number(row)
                    + " col=" + QByteArray::number(col)
                    + " input=" + input.toHex();
                QVERIFY2(batched.cellAt(row, col)->chars
                             == incremental.cellAt(row, col)->chars,
                         context.constData());
            }
        }
    }
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

namespace {

// 把字面量入队（BoundedByteQueue 现接收 ByteView）。
bool enqueueLiteral(NovaTerm::BoundedByteQueue& queue, const char* text,
                    int timeoutMs = 0)
{
    return queue.enqueue(
        NovaTerm::ByteView(text, NovaTerm::isize(std::strlen(text))), timeoutMs);
}

// 取出至多 maxBytes 字节为 QByteArray（take 现填充调用方缓冲）。
QByteArray takeBytes(NovaTerm::BoundedByteQueue& queue, NovaTerm::isize maxBytes,
                     int timeoutMs = 0)
{
    QByteArray buffer(maxBytes, Qt::Uninitialized);
    const NovaTerm::isize taken =
        queue.take(buffer.data(), maxBytes, timeoutMs);
    buffer.resize(taken);
    return buffer;
}

} // namespace

void TerminalCoreTests::boundedByteQueuePreservesOrderAndBackpressure()
{
    NovaTerm::BoundedByteQueue queue(8);

    QVERIFY(enqueueLiteral(queue, "abcdef"));
    QCOMPARE(takeBytes(queue, 4), QByteArrayLiteral("abcd"));
    QVERIFY(enqueueLiteral(queue, "WXYZ"));
    QCOMPARE(takeBytes(queue, 8), QByteArrayLiteral("efWXYZ"));

    QVERIFY(enqueueLiteral(queue, "12345678"));
    QVERIFY(!enqueueLiteral(queue, "x"));
    const auto statistics = queue.statistics();
    QCOMPARE(statistics.capacity, NovaTerm::isize(8));
    QCOMPARE(statistics.highWatermark, NovaTerm::isize(8));
    QVERIFY(statistics.producerWaits >= 1);
}

void TerminalCoreTests::boundedByteQueueWakesBlockedProducer()
{
    NovaTerm::BoundedByteQueue queue(8);
    QVERIFY(enqueueLiteral(queue, "12345678"));

    std::atomic<bool> completed{false};
    std::thread producer([&]() {
        const bool accepted = enqueueLiteral(queue, "x", -1);
        completed.store(accepted, std::memory_order_release);
    });

    QTRY_VERIFY_WITH_TIMEOUT(queue.statistics().producerWaits > 0, 1000);
    QVERIFY(!completed.load(std::memory_order_acquire));
    QCOMPARE(takeBytes(queue, 1), QByteArrayLiteral("1"));
    producer.join();
    QVERIFY(completed.load(std::memory_order_acquire));
    QCOMPARE(takeBytes(queue, 8), QByteArrayLiteral("2345678x"));
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

// 核心 KeyMapper 不依赖 Qt：直接以核心输入类型（Key/KeyModifier/码点）
// 验证到 libvterm 键码/修饰符/控制字符的映射。此前 KeyMapper 零覆盖。
void TerminalCoreTests::keyMapperMapsSpecialKeysAndModifiers()
{
    VTermKey key = VTERM_KEY_NONE;
    QVERIFY(KeyMapper::keyToVTermKey(NovaTerm::Key::Enter, key));
    QCOMPARE(key, VTERM_KEY_ENTER);
    QVERIFY(KeyMapper::keyToVTermKey(NovaTerm::Key::Up, key));
    QCOMPARE(key, VTERM_KEY_UP);
    QVERIFY(KeyMapper::keyToVTermKey(NovaTerm::Key::PageDown, key));
    QCOMPARE(key, VTERM_KEY_PAGEDOWN);
    QVERIFY(KeyMapper::keyToVTermKey(NovaTerm::Key::F5, key));
    QCOMPARE(key, static_cast<VTermKey>(VTERM_KEY_FUNCTION(5)));
    // None 与文本键无映射，调用方据此回退到字符输入路径。
    QVERIFY(!KeyMapper::keyToVTermKey(NovaTerm::Key::None, key));

    using NovaTerm::KeyModifier;
    QCOMPARE(int(KeyMapper::modToVTermMod(KeyModifier::None)),
             int(VTERM_MOD_NONE));
    QCOMPARE(int(KeyMapper::modToVTermMod(KeyModifier::Ctrl)),
             int(VTERM_MOD_CTRL));
    QCOMPARE(int(KeyMapper::modToVTermMod(
                 KeyModifier::Ctrl | KeyModifier::Shift | KeyModifier::Alt)),
             int(VTERM_MOD_CTRL | VTERM_MOD_SHIFT | VTERM_MOD_ALT));
}

void TerminalCoreTests::keyMapperMapsControlCharacters()
{
    uint32_t codepoint = 0xFFFF;
    // Ctrl+A..Z → 0x01..0x1A（大小写均可）。
    QVERIFY(KeyMapper::codepointToControlCharacter('A', codepoint));
    QCOMPARE(codepoint, uint32_t(0x01));
    QVERIFY(KeyMapper::codepointToControlCharacter('c', codepoint));
    QCOMPARE(codepoint, uint32_t(0x03));  // Ctrl+C → ETX
    // 符号控制字符。
    QVERIFY(KeyMapper::codepointToControlCharacter('[', codepoint));
    QCOMPARE(codepoint, uint32_t(0x1B));  // ESC
    QVERIFY(KeyMapper::codepointToControlCharacter('?', codepoint));
    QCOMPARE(codepoint, uint32_t(0x7F));  // DEL
    QVERIFY(KeyMapper::codepointToControlCharacter(' ', codepoint));
    QCOMPARE(codepoint, uint32_t(0x00));  // Ctrl+Space → NUL
    // 无对应控制字符的普通码点返回 false。
    QVERIFY(!KeyMapper::codepointToControlCharacter('1', codepoint));
}

// AltGr（Windows 上报为 Ctrl+Alt）产生的可打印字符必须原样发送，不能被
// 当成 Ctrl 控制字符。回归 AltGr+Q=@ 误发 0x11(XOFF) 冻结显示的缺陷。
void TerminalCoreTests::altGrProducesPrintableCharacterNotControlCode()
{
    TerminalCore core(20, 4);
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    // 德语键盘 AltGr+Q 产生 '@'：物理键仍是 Q，text 是 "@"，修饰符是
    // Ctrl|Alt（Qt 在 Windows 上如此上报 AltGr）。
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Q,
                    Qt::ControlModifier | Qt::AltModifier, QStringLiteral("@"));
    core.processKeyPress(&event);
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!outputSpy.isEmpty());

    QByteArray output;
    for (const auto& arguments : outputSpy)
        output += arguments.at(0).toByteArray();
    QCOMPARE(output, QByteArrayLiteral("@"));
    // 断言没有误发 XOFF（0x11 = Ctrl+Q）。
    QVERIFY(!output.contains('\x11'));
}

// Shift+Space 必须发送普通空格，而非 libvterm 对 Shift+Space 特判产生的
// CSI 32;2u。
void TerminalCoreTests::shiftSpaceSendsPlainSpace()
{
    TerminalCore core(20, 4);
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    QKeyEvent event(QEvent::KeyPress, Qt::Key_Space,
                    Qt::ShiftModifier, QStringLiteral(" "));
    core.processKeyPress(&event);
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!outputSpy.isEmpty());

    QByteArray output;
    for (const auto& arguments : outputSpy)
        output += arguments.at(0).toByteArray();
    QCOMPARE(output, QByteArrayLiteral(" "));
}

// 单独的 Alt+字母仍应作为 meta，即以 ESC 前缀发送 —— 确认 AltGr 修复没有
// 误伤真正的 Alt 组合。
void TerminalCoreTests::altLetterSendsMetaEscapePrefix()
{
    TerminalCore core(20, 4);
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    QKeyEvent event(QEvent::KeyPress, Qt::Key_A,
                    Qt::AltModifier, QStringLiteral("a"));
    core.processKeyPress(&event);
    QVERIFY(core.waitForIdle());
    QTRY_VERIFY(!outputSpy.isEmpty());

    QByteArray output;
    for (const auto& arguments : outputSpy)
        output += arguments.at(0).toByteArray();
    QCOMPARE(output, QByteArrayLiteral("\x1b""a"));
}

QTEST_GUILESS_MAIN(TerminalCoreTests)

#include "TerminalCoreTests.moc"
