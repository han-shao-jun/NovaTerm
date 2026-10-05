// 常见终端操作用例：参考 esctest2 / vttest 的覆盖面（光标移动、擦除、
// 插入删除、SGR、设备应答、制表、回车覆写、自动换行、组合字符）。
// 只断言单元格字符、属性与光标位置，与 esctest2 的可自动化范围一致。
#include "core/terminal/TerminalCore.h"

#include <QSignalSpy>
#include <QtTest>

class TerminalOpsTests final : public QObject
{
    Q_OBJECT

private slots:
    void cursorPositionAndRelativeMoves();
    void cursorMovesClampAtScreenEdges();
    void eraseInDisplayVariants();
    void eraseInLineVariants();
    void insertAndDeleteCharacters();
    void insertAndDeleteLines();
    void eraseCharactersDoesNotShift();
    void sgrAttributesSetAndReset();
    void deviceStatusReportReturnsCursorPosition();
    void primaryDeviceAttributesAnswered();
    void horizontalTabUsesDefaultStops();
    void backspaceAndCarriageReturnOverwrite();
    void autowrapAtRightMargin();
    void autowrapDisabledOverwritesLastColumn();
    void insertModeShiftsExistingText();
    void combiningCharacterJoinsPreviousCell();
    void fullResetClearsScreenAndAttributes();

    // 以下移植自 Windows Terminal src/host/ut_host/ScreenBufferTests.cpp
    void delayedWrapReset_data();
    void delayedWrapReset();
    void cursorNextPreviousLine();
    void screenAlignmentPattern();
    void lineFeedsInsideScrollMargins();
    void tabStopSetClearAndReverse();
    void cursorSaveRestore();
};

namespace {

uint32_t charAt(const TerminalCore& core, int row, int col)
{
    NovaTerm::Cell cell;
    if (!core.getCell(row, col, cell))
        return 0xFFFFFFFFu;
    return cell.chars[0] == uint32_t(' ') ? 0 : cell.chars[0];
}

QString rowText(const TerminalCore& core, int row)
{
    QString text;
    for (int col = 0; col < core.columns(); ++col) {
        const uint32_t ch = charAt(core, row, col);
        text += ch == 0 ? QLatin1Char('.') : QChar(char16_t(ch));
    }
    return text;
}

void feed(TerminalCore& core, const QByteArray& bytes)
{
    core.writeInput(bytes);
    QVERIFY(core.waitForIdle());
}

QByteArray collect(const QSignalSpy& spy)
{
    QByteArray out;
    for (const QList<QVariant>& args : spy)
        out += args.at(0).toByteArray();
    return out;
}

} // namespace

void TerminalOpsTests::cursorPositionAndRelativeMoves()
{
    TerminalCore core(20, 6);
    feed(core, QByteArrayLiteral("\x1b[3;5H"));   // CUP 1-based
    QCOMPARE(core.cursorState().position.row, 2);
    QCOMPARE(core.cursorState().position.col, 4);
    feed(core, QByteArrayLiteral("\x1b[2A\x1b[3C")); // CUU 2, CUF 3
    QCOMPARE(core.cursorState().position.row, 0);
    QCOMPARE(core.cursorState().position.col, 7);
    feed(core, QByteArrayLiteral("\x1b[4B\x1b[5D")); // CUD 4, CUB 5
    QCOMPARE(core.cursorState().position.row, 4);
    QCOMPARE(core.cursorState().position.col, 2);
    feed(core, QByteArrayLiteral("\x1b[10G"));        // CHA
    QCOMPARE(core.cursorState().position.col, 9);
    feed(core, QByteArrayLiteral("\x1b[2d"));         // VPA
    QCOMPARE(core.cursorState().position.row, 1);
    feed(core, QByteArrayLiteral("\x1b[H"));          // home
    QCOMPARE(core.cursorState().position.row, 0);
    QCOMPARE(core.cursorState().position.col, 0);
}

void TerminalOpsTests::cursorMovesClampAtScreenEdges()
{
    TerminalCore core(20, 6);
    feed(core, QByteArrayLiteral("\x1b[99;99H"));
    QCOMPARE(core.cursorState().position.row, 5);
    QCOMPARE(core.cursorState().position.col, 19);
    feed(core, QByteArrayLiteral("\x1b[99A\x1b[99D"));
    QCOMPARE(core.cursorState().position.row, 0);
    QCOMPARE(core.cursorState().position.col, 0);
}

void TerminalOpsTests::eraseInDisplayVariants()
{
    TerminalCore core(5, 3);
    const QByteArray fill = QByteArrayLiteral("\x1b[HAAAAA\r\nBBBBB\r\nCCCCC");

    feed(core, fill + QByteArrayLiteral("\x1b[2;3H\x1b[0J")); // ED 0
    QCOMPARE(rowText(core, 0), QStringLiteral("AAAAA"));
    QCOMPARE(rowText(core, 1), QStringLiteral("BB..."));
    QCOMPARE(rowText(core, 2), QStringLiteral("....."));

    feed(core, fill + QByteArrayLiteral("\x1b[2;3H\x1b[1J")); // ED 1
    QCOMPARE(rowText(core, 0), QStringLiteral("....."));
    QCOMPARE(rowText(core, 1), QStringLiteral("...BB"));
    QCOMPARE(rowText(core, 2), QStringLiteral("CCCCC"));

    feed(core, fill + QByteArrayLiteral("\x1b[2;3H\x1b[2J")); // ED 2
    for (int row = 0; row < 3; ++row)
        QCOMPARE(rowText(core, row), QStringLiteral("....."));
    // ED 2 不移动光标
    QCOMPARE(core.cursorState().position.row, 1);
    QCOMPARE(core.cursorState().position.col, 2);
}

void TerminalOpsTests::eraseInLineVariants()
{
    TerminalCore core(6, 2);
    feed(core, QByteArrayLiteral("\x1b[HABCDEF\x1b[1;3H\x1b[K"));
    QCOMPARE(rowText(core, 0), QStringLiteral("AB...."));
    feed(core, QByteArrayLiteral("\x1b[HABCDEF\x1b[1;3H\x1b[1K"));
    QCOMPARE(rowText(core, 0), QStringLiteral("...DEF"));
    feed(core, QByteArrayLiteral("\x1b[HABCDEF\x1b[1;3H\x1b[2K"));
    QCOMPARE(rowText(core, 0), QStringLiteral("......"));
}

void TerminalOpsTests::insertAndDeleteCharacters()
{
    TerminalCore core(6, 2);
    feed(core, QByteArrayLiteral("\x1b[HABCDEF\x1b[1;2H\x1b[2@")); // ICH
    QCOMPARE(rowText(core, 0), QStringLiteral("A..BCD"));
    feed(core, QByteArrayLiteral("\x1b[HABCDEF\x1b[1;2H\x1b[2P")); // DCH
    QCOMPARE(rowText(core, 0), QStringLiteral("ADEF.."));
}

void TerminalOpsTests::insertAndDeleteLines()
{
    TerminalCore core(3, 4);
    const QByteArray fill = QByteArrayLiteral("\x1b[H111\r\n222\r\n333\r\n444");
    feed(core, fill + QByteArrayLiteral("\x1b[2;1H\x1b[L"));   // IL
    QCOMPARE(rowText(core, 0), QStringLiteral("111"));
    QCOMPARE(rowText(core, 1), QStringLiteral("..."));
    QCOMPARE(rowText(core, 2), QStringLiteral("222"));
    QCOMPARE(rowText(core, 3), QStringLiteral("333"));

    feed(core, fill + QByteArrayLiteral("\x1b[2;1H\x1b[2M"));  // DL
    QCOMPARE(rowText(core, 0), QStringLiteral("111"));
    QCOMPARE(rowText(core, 1), QStringLiteral("444"));
    QCOMPARE(rowText(core, 2), QStringLiteral("..."));
    QCOMPARE(rowText(core, 3), QStringLiteral("..."));
}

void TerminalOpsTests::eraseCharactersDoesNotShift()
{
    TerminalCore core(6, 2);
    feed(core, QByteArrayLiteral("\x1b[HABCDEF\x1b[1;2H\x1b[3X")); // ECH
    QCOMPARE(rowText(core, 0), QStringLiteral("A...EF"));
    QCOMPARE(core.cursorState().position.col, 1);
}

void TerminalOpsTests::sgrAttributesSetAndReset()
{
    TerminalCore core(10, 2);
    feed(core, QByteArrayLiteral("\x1b[1;3;4;7;9mA\x1b[0mB"));
    NovaTerm::Cell styled;
    QVERIFY(core.getCell(0, 0, styled));
    QVERIFY(styled.attributes.bold);
    QVERIFY(styled.attributes.italic);
    QVERIFY(styled.attributes.underline);
    QVERIFY(styled.attributes.reverse);
    QVERIFY(styled.attributes.strike);

    NovaTerm::Cell plain;
    QVERIFY(core.getCell(0, 1, plain));
    QCOMPARE(plain.chars[0], uint32_t('B'));
    QVERIFY(!plain.attributes.bold);
    QVERIFY(!plain.attributes.italic);
    QVERIFY(!plain.attributes.underline);
    QVERIFY(!plain.attributes.reverse);
    QVERIFY(!plain.attributes.strike);
}

void TerminalOpsTests::deviceStatusReportReturnsCursorPosition()
{
    TerminalCore core(20, 6);
    feed(core, QByteArrayLiteral("\x1b[4;7H"));
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    feed(core, QByteArrayLiteral("\x1b[6n"));
    QTRY_VERIFY(!outputSpy.isEmpty());
    QCOMPARE(collect(outputSpy), QByteArrayLiteral("\x1b[4;7R"));
}

void TerminalOpsTests::primaryDeviceAttributesAnswered()
{
    TerminalCore core(20, 6);
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    feed(core, QByteArrayLiteral("\x1b[c"));
    QTRY_VERIFY(!outputSpy.isEmpty());
    const QByteArray reply = collect(outputSpy);
    QVERIFY2(reply.startsWith("\x1b[?") && reply.endsWith('c'), reply.constData());
}

void TerminalOpsTests::horizontalTabUsesDefaultStops()
{
    TerminalCore core(40, 2);
    feed(core, QByteArrayLiteral("a\tb\tc"));
    QCOMPARE(charAt(core, 0, 0), uint32_t('a'));
    QCOMPARE(charAt(core, 0, 8), uint32_t('b'));
    QCOMPARE(charAt(core, 0, 16), uint32_t('c'));
}

void TerminalOpsTests::backspaceAndCarriageReturnOverwrite()
{
    TerminalCore core(10, 2);
    feed(core, QByteArrayLiteral("hello\b\bLO"));
    QCOMPARE(rowText(core, 0).left(5), QStringLiteral("helLO"));
    feed(core, QByteArrayLiteral("\rJ"));       // 进度条式回车覆写
    QCOMPARE(rowText(core, 0).left(5), QStringLiteral("JelLO"));
}

void TerminalOpsTests::autowrapAtRightMargin()
{
    TerminalCore core(4, 3);
    feed(core, QByteArrayLiteral("ABCDEF"));
    QCOMPARE(rowText(core, 0), QStringLiteral("ABCD"));
    QCOMPARE(rowText(core, 1), QStringLiteral("EF.."));
    QCOMPARE(core.cursorState().position.row, 1);
    QCOMPARE(core.cursorState().position.col, 2);
}

void TerminalOpsTests::autowrapDisabledOverwritesLastColumn()
{
    TerminalCore core(4, 3);
    feed(core, QByteArrayLiteral("\x1b[?7lABCDEF"));
    QCOMPARE(rowText(core, 0), QStringLiteral("ABCF"));
    QCOMPARE(rowText(core, 1), QStringLiteral("...."));
}

void TerminalOpsTests::insertModeShiftsExistingText()
{
    TerminalCore core(8, 2);
    feed(core, QByteArrayLiteral("ABCD\x1b[1;2H\x1b[4hxy\x1b[4l"));
    QCOMPARE(rowText(core, 0), QStringLiteral("AxyBCD.."));
}

void TerminalOpsTests::combiningCharacterJoinsPreviousCell()
{
    TerminalCore core(10, 2);
    feed(core, QByteArrayLiteral("e\xcc\x81x"));   // e + U+0301
    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QCOMPARE(cell.chars[0], uint32_t('e'));
    QCOMPARE(cell.chars[1], uint32_t(0x0301));
    QCOMPARE(charAt(core, 0, 1), uint32_t('x'));
}

void TerminalOpsTests::fullResetClearsScreenAndAttributes()
{
    TerminalCore core(10, 3);
    feed(core, QByteArrayLiteral("\x1b[1;31mdirty\x1b[?25l\x1b" "c"));   // RIS
    QCOMPARE(rowText(core, 0), QStringLiteral(".........."));
    QCOMPARE(core.cursorState().position.row, 0);
    QCOMPARE(core.cursorState().position.col, 0);
    QVERIFY(core.cursorState().visible);
    feed(core, QByteArrayLiteral("Z"));
    NovaTerm::Cell cell;
    QVERIFY(core.getCell(0, 0, cell));
    QVERIFY(!cell.attributes.bold);
    QCOMPARE(cell.foreground.type, NovaTerm::ColorType::Default);
}

// ── 移植自 Windows Terminal ScreenBufferTests ──────────────────────

// WT DelayedWrapReset：DEC STD 070 D-13 列出的、必须清除"行尾延迟换行"
// 标志的操作。在最后一列写字符后光标停在行尾并挂起换行；执行操作后
// 再写一个字符，它必须落在期望位置，而不是先换到下一行。
// 屏幕 20×10，起点 (row 5, col 19)。WT 表中的 DECCOLM 两项未移植：
// NovaTerm 不支持 132 列切换。
void TerminalOpsTests::delayedWrapReset_data()
{
    QTest::addColumn<QByteArray>("sequence");
    QTest::addColumn<int>("row");
    QTest::addColumn<int>("col");

    QTest::newRow("DECSTBM") << QByteArray("\x1b[1;10r") << 0 << 0;
    QTest::newRow("DECSLRM") << QByteArray("\x1b[?69h\x1b[1;10s") << 0 << 0;
    QTest::newRow("DECSWL") << QByteArray("\x1b#5") << 5 << 19;
    QTest::newRow("DECDWL") << QByteArray("\x1b#6") << 5 << 9;
    QTest::newRow("DECDHL top") << QByteArray("\x1b#3") << 5 << 9;
    QTest::newRow("DECDHL bottom") << QByteArray("\x1b#4") << 5 << 9;
    QTest::newRow("DECOM set") << QByteArray("\x1b[?6h") << 0 << 0;
    QTest::newRow("DECOM reset") << QByteArray("\x1b[?6l") << 0 << 0;
    QTest::newRow("DECAWM reset") << QByteArray("\x1b[?7l") << 5 << 19;
    QTest::newRow("CUU") << QByteArray("\x1b[A") << 4 << 19;
    QTest::newRow("CUD") << QByteArray("\x1b[B") << 6 << 19;
    QTest::newRow("CUF") << QByteArray("\x1b[C") << 5 << 19;
    QTest::newRow("CUB") << QByteArray("\x1b[D") << 5 << 18;
    QTest::newRow("CUP") << QByteArray("\x1b[3;7H") << 2 << 6;
    QTest::newRow("HVP") << QByteArray("\x1b[3;7f") << 2 << 6;
    QTest::newRow("BS") << QByteArray("\b") << 5 << 18;
    QTest::newRow("LF") << QByteArray("\n") << 6 << 19;
    QTest::newRow("VT") << QByteArray("\v") << 6 << 19;
    QTest::newRow("FF") << QByteArray("\f") << 6 << 19;
    QTest::newRow("CR") << QByteArray("\r") << 5 << 0;
    QTest::newRow("IND") << QByteArray("\x1b" "D") << 6 << 19;
    QTest::newRow("RI") << QByteArray("\x1bM") << 4 << 19;
    QTest::newRow("NEL") << QByteArray("\x1b" "E") << 6 << 0;
    QTest::newRow("ECH") << QByteArray("\x1b[X") << 5 << 19;
    QTest::newRow("DCH") << QByteArray("\x1b[P") << 5 << 19;
    QTest::newRow("ICH") << QByteArray("\x1b[@") << 5 << 19;
    QTest::newRow("EL") << QByteArray("\x1b[K") << 5 << 19;
    QTest::newRow("DECSEL") << QByteArray("\x1b[?K") << 5 << 19;
    QTest::newRow("DL") << QByteArray("\x1b[M") << 5 << 0;
    QTest::newRow("IL") << QByteArray("\x1b[L") << 5 << 0;
    QTest::newRow("ED") << QByteArray("\x1b[J") << 5 << 19;
    QTest::newRow("ED all") << QByteArray("\x1b[2J") << 5 << 19;
    QTest::newRow("ED scrollback") << QByteArray("\x1b[3J") << 5 << 19;
    QTest::newRow("DECSED") << QByteArray("\x1b[?J") << 5 << 19;
}

void TerminalOpsTests::delayedWrapReset()
{
    QFETCH(QByteArray, sequence);
    QFETCH(int, row);
    QFETCH(int, col);

    TerminalCore core(20, 10);
    feed(core, QByteArrayLiteral("\x1b[6;20Ha"));
    QCOMPARE(core.cursorState().position.row, 5);
    QCOMPARE(core.cursorState().position.col, 19);

    feed(core, sequence + QByteArrayLiteral("X"));
    QVERIFY2(charAt(core, row, col) == uint32_t('X'),
             qPrintable(QStringLiteral("row %1 = \"%2\", next row = \"%3\"")
                            .arg(row)
                            .arg(rowText(core, row), rowText(core, row + 1))));
}

// WT CursorNextPreviousLine：CNL/CPL 回到列首；在滚动区内被上下边距钳住，
// 并回到左边距；区外不受边距影响。
void TerminalOpsTests::cursorNextPreviousLine()
{
    TerminalCore core(40, 20);
    feed(core, QByteArrayLiteral("\x1b[11;11H\x1b[5E"));
    QCOMPARE(core.cursorState().position.row, 15);
    QCOMPARE(core.cursorState().position.col, 0);
    feed(core, QByteArrayLiteral("\x1b[11;11H\x1b[5F"));
    QCOMPARE(core.cursorState().position.row, 5);
    QCOMPARE(core.cursorState().position.col, 0);

    // 列边距 11..30、行边距 9..13（1-based）
    feed(core, QByteArrayLiteral("\x1b[?69h\x1b[11;30s\x1b[9;13r"));
    feed(core, QByteArrayLiteral("\x1b[11;16H\x1b[5E"));
    QCOMPARE(core.cursorState().position.row, 12);
    QCOMPARE(core.cursorState().position.col, 10);
    feed(core, QByteArrayLiteral("\x1b[11;16H\x1b[5F"));
    QCOMPARE(core.cursorState().position.row, 8);
    QCOMPARE(core.cursorState().position.col, 10);
    feed(core, QByteArrayLiteral("\x1b[14;1H\x1b[5E"));
    QCOMPARE(core.cursorState().position.row, 18);
    QCOMPARE(core.cursorState().position.col, 0);
    feed(core, QByteArrayLiteral("\x1b[8;1H\x1b[5F"));
    QCOMPARE(core.cursorState().position.row, 2);
    QCOMPARE(core.cursorState().position.col, 0);
}

// WT ScreenAlignmentPattern：DECALN 用 E 填满屏幕、光标归位、清除滚动区。
void TerminalOpsTests::screenAlignmentPattern()
{
    TerminalCore core(10, 4);
    feed(core, QByteArrayLiteral("zzzz\x1b[2;3r\x1b[3;5H\x1b#8"));
    for (int row = 0; row < 4; ++row)
        QCOMPARE(rowText(core, row), QStringLiteral("EEEEEEEEEE"));
    QCOMPARE(core.cursorState().position.row, 0);
    QCOMPARE(core.cursorState().position.col, 0);

    // 滚动区已清除：在最后一行 LF 会整屏上滚，最后一行变空。
    // 若仍保留 2;3 区域，光标在区外的最后一行，LF 不滚动。
    feed(core, QByteArrayLiteral("\x1b[4;1H\n"));
    QCOMPARE(rowText(core, 3), QStringLiteral(".........."));
}

// WT LineFeedEscapeSequences 的滚动区部分：在下边距执行 IND/NEL 只滚动
// 区内内容，光标不越出区域；RI 在上边距反向滚动。
void TerminalOpsTests::lineFeedsInsideScrollMargins()
{
    QByteArray fill = QByteArrayLiteral("\x1b[H");
    for (int i = 0; i < 8; ++i) {
        fill += QByteArray(20, char('0' + i));
        if (i < 7)
            fill += "\r\n";
    }

    {
        TerminalCore core(20, 8);
        feed(core, fill + QByteArrayLiteral("\x1b[3;6r\x1b[6;11H\x1b" "D"));
        QCOMPARE(core.cursorState().position.row, 5);
        QCOMPARE(core.cursorState().position.col, 10);
        QCOMPARE(charAt(core, 1, 0), uint32_t('1'));
        QCOMPARE(charAt(core, 2, 0), uint32_t('3'));
        QCOMPARE(rowText(core, 5), QString(20, QLatin1Char('.')));
        QCOMPARE(charAt(core, 6, 0), uint32_t('6'));
    }
    {
        TerminalCore core(20, 8);
        feed(core, fill + QByteArrayLiteral("\x1b[3;6r\x1b[6;11H\x1b" "E"));
        QCOMPARE(core.cursorState().position.row, 5);
        QCOMPARE(core.cursorState().position.col, 0);
        QCOMPARE(charAt(core, 2, 0), uint32_t('3'));
    }
    {
        TerminalCore core(20, 8);
        feed(core, fill + QByteArrayLiteral("\x1b[3;6r\x1b[3;11H\x1bM"));
        QCOMPARE(core.cursorState().position.row, 2);
        QCOMPARE(core.cursorState().position.col, 10);
        QCOMPARE(rowText(core, 2), QString(20, QLatin1Char('.')));
        QCOMPARE(charAt(core, 3, 0), uint32_t('2'));
        QCOMPARE(charAt(core, 5, 0), uint32_t('4'));
        QCOMPARE(charAt(core, 6, 0), uint32_t('6'));
    }
}

// WT TestAddTabStop / TestClearTabStop / TestGetReverseTab。
void TerminalOpsTests::tabStopSetClearAndReverse()
{
    TerminalCore core(40, 3);
    // TBC 3 清除全部：HT 走到最后一列
    feed(core, QByteArrayLiteral("\x1b[3g\r\t"));
    QCOMPARE(core.cursorState().position.col, 39);

    // HTS 设置自定义制表位
    feed(core, QByteArrayLiteral("\x1b[1;6H\x1bH\x1b[1;13H\x1bH\r\t"));
    QCOMPARE(core.cursorState().position.col, 5);
    feed(core, QByteArrayLiteral("\t"));
    QCOMPARE(core.cursorState().position.col, 12);

    // RIS 恢复默认 8 列制表位后，TBC 0 只清除当前列
    feed(core, QByteArrayLiteral("\x1b" "c\x1b[1;9H\x1b[g\r\t"));
    QCOMPARE(core.cursorState().position.col, 16);

    // CBT 反向制表
    feed(core, QByteArrayLiteral("\x1b" "c\x1b[1;20H\x1b[Z"));
    QCOMPARE(core.cursorState().position.col, 16);
    feed(core, QByteArrayLiteral("\x1b[2Z"));
    QCOMPARE(core.cursorState().position.col, 0);
}

// WT CursorSaveRestore：DECSC/DECRC 保存并恢复位置与画笔属性。
void TerminalOpsTests::cursorSaveRestore()
{
    TerminalCore core(20, 6);
    feed(core, QByteArrayLiteral("\x1b[3;5H\x1b[1m\x1b" "7\x1b[1;1H\x1b[0m\x1b" "8Q"));
    NovaTerm::Cell cell;
    QVERIFY(core.getCell(2, 4, cell));
    QCOMPARE(cell.chars[0], uint32_t('Q'));
    QVERIFY(cell.attributes.bold);
}

QTEST_GUILESS_MAIN(TerminalOpsTests)

#include "TerminalOpsTests.moc"
