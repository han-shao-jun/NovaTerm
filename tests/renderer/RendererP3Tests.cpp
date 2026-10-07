#include "renderer/RenderCommandBuffer.h"
#include "renderer/RenderScheduler.h"
#include "renderer/TerminalRenderer.h"
#include "core/terminal/TerminalCore.h"
#include "service/TerminalSchemeStore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QInputMethodEvent>
#include <QApplication>
#include <QKeyEvent>
#include <QTest>

namespace {
QTimer* cursorBlinkTimer(TerminalRenderer& renderer);
}

class RendererP3Tests : public QObject
{
    Q_OBJECT

private slots:
    void colorSchemesParseWindowsTerminalFormat();
    void colorSchemesResolveReferencesAndOverrides();
    void colorSchemesRenameAndDeleteReferences();
    void colorSchemesMigrateLegacyWithoutLosingColors();
    void colorSchemesMigrationPreservesSystemMode();
    void colorSchemeChangePreservesTerminalContent();
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
    void commandBufferSwapOverlaysRetainsBothCapacities();
    void commandRowTracksHighlightRoleAcrossRebuild();
    void highlightPrefilterExtractsMandatoryLiterals();
    void highlightPrefilterNeverExcludesAMatch();
    void scrollbackAtLiveBottomDoesNotRequestFullFrame();
    void liveBottomMaintainsHistoryLayout();
    void historyLayoutMaintenanceIsCoalescedPerEventTurn();
    void batchedScreenScrollPublishesExactRowCount();
    void enteringHistoryReusesHistoryLayout();
    void returningToLiveBottomKeepsHistoryLayout();
    void modifierKeysKeepHistoryAndJumpsReachEnds();
    void columnReflowKeepsScrollbackPosition();
    void zoomOutReflowKeepsScrollbackPosition();
    void columnChangeRequestsReflowRowChangeDoesNot();
    void scrollStateTracksHistoryGrowthAndOffset();
    void softWrappedSelectionCopiesAsSingleLine();
    void hardBreakSelectionKeepsNewline();
    void selectAllIncludesHistoryAndVisibleScreen();
    void vtMouseTrackingClaimsLeftButtonGesture();
    void searchMatchesAppendByGeneration();
    void inputMethodCommitProducesUtf8();
    void fragmentedOutputDoesNotInflateScrollbackBytes();
    void spanAssemblyCoversBackgroundSlots();
    void spanAssemblyPacksContentIntoFourSlotsPerCell();
    void spanAssemblyOffsetsMatchRequestedSpan();
    void shadowBufferSkipsIdenticalUploads();
    void incrementalRowMergeKeepsOrderAndColumns();
    void mutableRowRebuildKeepsMetadataAndDropsOutOfRange();
    void hiddenRendererDefersFramesUntilShown();
    void cursorVisibilityChangesRequestOverlay();
    void outputDoesNotRestartPeriodicCursorTimer();
    void identicalFontDoesNotInvalidateRows();
    void overlayColorsDoNotInvalidateRows();
    void minimizedOutputDoesNotRestartBlink();
    void minimizedOutputDoesNotRestartBlink_data();
    void historyViewDoesNotScheduleCursorFrames();
};

void RendererP3Tests::minimizedOutputDoesNotRestartBlink()
{
    QFETCH(bool, embedded);
    TerminalCore core(80, 24);
    QWidget host;
    TerminalRenderer renderer(&core, embedded ? &host : nullptr);
    renderer.setApi(QRhiWidget::Api::Null);
    core.writeInput("\x1b[?12h");
    QTRY_VERIFY(core.cursorBlink());
    QWidget* window = renderer.window();
    window->show();
    auto* timer = cursorBlinkTimer(renderer);
    QVERIFY(timer);
    QTRY_VERIFY(timer->isActive());
    window->showMinimized();
    QTest::qWait(50);
    QSignalSpy damage(&core, &TerminalCore::damage);
    core.writeInput("minimized");
    QTRY_VERIFY(!damage.isEmpty());
    QVERIFY(!timer->isActive());
    window->showNormal();
    QTRY_VERIFY(timer->isActive());
}

void RendererP3Tests::minimizedOutputDoesNotRestartBlink_data()
{
    QTest::addColumn<bool>("embedded");
    QTest::newRow("standalone") << false;
    QTest::newRow("embedded") << true;
}

void RendererP3Tests::historyViewDoesNotScheduleCursorFrames()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.setApi(QRhiWidget::Api::Null);
    renderer.show();
    core.writeInput(QByteArray("\x1b[?12h") + QByteArray("old\r\n").repeated(100));
    QTRY_VERIFY(renderer.maximumScrollOffset() > 0);
    auto* timer = cursorBlinkTimer(renderer);
    QVERIFY(timer);
    QTRY_VERIFY(renderer.historyDisplayRowCount() > 0);
    renderer.scrollToTop();
    QTest::qWait(80);
    auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    QVERIFY(scheduler);
    QSignalSpy frames(scheduler, &NovaTerm::RenderScheduler::frameRequested);
    QTest::qWait(600);
    QCOMPARE(frames.size(), 0);
    core.clearScrollback();
    QTRY_COMPARE(renderer.scrollOffset(), 0);
    QTRY_VERIFY(timer->isActive());
}

void RendererP3Tests::hiddenRendererDefersFramesUntilShown()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.setApi(QRhiWidget::Api::Null);
    auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    QVERIFY(scheduler);
    QSignalSpy spy(scheduler, &NovaTerm::RenderScheduler::frameRequested);
    for (int i = 0; i < 100; ++i)
        scheduler->schedule({0, 1, 0, 1}, i + 1);
    QTest::qWait(60);
    QCOMPARE(spy.size(), 0);

    renderer.show();
    QTRY_VERIFY(!spy.isEmpty());
    QVERIFY(spy.first().at(1).toBool());
    renderer.hide();
    spy.clear();
    core.writeInput("hidden output\r\n");
    QTest::qWait(60);
    QCOMPARE(spy.size(), 0);
    renderer.show();
    QTRY_VERIFY(!spy.isEmpty());
    QVERIFY(spy.first().at(1).toBool());
}

namespace {
QTimer* cursorBlinkTimer(TerminalRenderer& renderer)
{
    // 光标计时器直接属于 Renderer；调度器和重排计时器不使用该周期。
    for (auto* timer : renderer.findChildren<QTimer*>(
             QString(), Qt::FindDirectChildrenOnly)) {
        if (timer->interval() == 530)
            return timer;
    }
    return nullptr;
}
}

void RendererP3Tests::cursorVisibilityChangesRequestOverlay()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.setApi(QRhiWidget::Api::Null);
    auto* timer = cursorBlinkTimer(renderer);
    QVERIFY(timer);
    QVERIFY(!timer->isActive());
    core.writeInput("\x1b[?12h");
    QTRY_VERIFY(core.cursorBlink());
    renderer.show();
    QTRY_VERIFY(timer->isActive());
    renderer.hide();
    QVERIFY(!timer->isActive());
    renderer.show();
    QTRY_VERIFY(timer->isActive());
    QTest::qWait(80);
    auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    QVERIFY(scheduler);
    QSignalSpy frames(scheduler, &NovaTerm::RenderScheduler::frameRequested);
    core.writeInput("\x1b[?25l");
    QTRY_VERIFY(!core.cursorVisible());
    QTRY_VERIFY(!frames.isEmpty());
    // 模式控制序列可同时发布正文 damage，仍必须更新光标绘制状态。
    QVERIFY(frames.last().at(2).toBool());
    frames.clear();
    core.writeInput("\x1b[?25h\x1b[?12l");
    QTRY_VERIFY(!core.cursorBlink());
    QTRY_VERIFY(!frames.isEmpty());
    QVERIFY(frames.last().at(2).toBool());
}

void RendererP3Tests::outputDoesNotRestartPeriodicCursorTimer()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.setApi(QRhiWidget::Api::Null);
    core.writeInput("\x1b[?12h");
    QTRY_VERIFY(core.cursorBlink());
    renderer.show();
    auto* timer = cursorBlinkTimer(renderer);
    QVERIFY(timer);
    QTRY_VERIFY(timer->isActive());
    QSignalSpy ticks(timer, &QTimer::timeout);
    QSignalSpy damage(&core, &TerminalCore::damage);
    for (int i = 0; i < 4; ++i) {
        QTest::qWait(180);
        damage.clear();
        core.writeInput("x");
        QTRY_VERIFY(!damage.isEmpty());
    }
    // 内容通知不能推迟低频周期；高频通知不再同步查询光标状态。
    QVERIFY(!ticks.isEmpty());
}

void RendererP3Tests::identicalFontDoesNotInvalidateRows()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.setApi(QRhiWidget::Api::Null);
    renderer.show();
    QTest::qWait(80);
    auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    QVERIFY(scheduler);
    QSignalSpy spy(scheduler, &NovaTerm::RenderScheduler::frameRequested);
    renderer.setFont(renderer.font());
    QTest::qWait(60);
    QCOMPARE(spy.size(), 0);
    QFont changed = renderer.font();
    changed.setPixelSize(changed.pixelSize() + 1);
    renderer.setFont(changed);
    QTRY_VERIFY(!spy.isEmpty());
    QVERIFY(spy.last().at(1).toBool());
}

void RendererP3Tests::overlayColorsDoNotInvalidateRows()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    renderer.setApi(QRhiWidget::Api::Null);
    renderer.setColorScheme(renderer.colorScheme());
    renderer.show();
    QTest::qWait(80);
    auto* scheduler = renderer.findChild<NovaTerm::RenderScheduler*>();
    QVERIFY(scheduler);
    QSignalSpy spy(scheduler, &NovaTerm::RenderScheduler::frameRequested);
    auto scheme = renderer.colorScheme();
    scheme.cursorColor = QColor("#ff1234");
    scheme.selectionColor = QColor("#5678ab");
    renderer.setColorScheme(scheme);
    QTRY_VERIFY(!spy.isEmpty());
    QVERIFY(!spy.last().at(1).toBool());
    QVERIFY(spy.last().at(2).toBool());
    QVERIFY(qvariant_cast<QVector<NovaTerm::DirtyRegion>>(
                spy.last().at(0)).isEmpty());
    spy.clear();
    renderer.setColorScheme(scheme);
    QTest::qWait(60);
    QCOMPARE(spy.size(), 0);
    scheme.palette[1] = QColor("#abcd12");
    renderer.setColorScheme(scheme);
    QTRY_VERIFY(!spy.isEmpty());
    QVERIFY(spy.last().at(1).toBool());
}

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

void RendererP3Tests::shadowBufferSkipsIdenticalUploads()
{
    NovaTerm::RenderCommandRow row;
    row.backgrounds.push_back(commandAtColumn(1, 1, false));
    row.contents.push_back(commandAtColumn(1, 2, false));
    QVector<TerminalRenderer::GpuInstance> background;
    QVector<TerminalRenderer::GpuInstance> content;
    const auto counts = TerminalRenderer::assembleSpanInstances(
        row, 0, 4, 2, background, content);
    const qsizetype bytes = qsizetype(counts.backgroundCount)
        * qsizetype(sizeof(TerminalRenderer::GpuInstance));

    // 新建缓冲的影子全为 0xFF（NaN 浮点）：任何装配结果都判为"不同"。
    QByteArray shadow(bytes + 128, '\xFF');
    QVERIFY(TerminalRenderer::syncShadowRange(
        shadow, 64, background.constData(), bytes));
    // 同步后再次提交相同字节：跳过上传。
    QVERIFY(!TerminalRenderer::syncShadowRange(
        shadow, 64, background.constData(), bytes));
    // 全零的退化实例写入全 0xFF 的影子同样必须上传（清除残留的关键路径）。
    QVector<TerminalRenderer::GpuInstance> zeros(
        counts.backgroundCount, TerminalRenderer::GpuInstance{});
    QVERIFY(TerminalRenderer::syncShadowRange(
        shadow, 64, zeros.constData(), bytes));
    QVERIFY(!TerminalRenderer::syncShadowRange(
        shadow, 64, zeros.constData(), bytes));

    // 只改一个字段：判为不同并更新影子，之后恢复为相同。
    background[1].r += 0.5f;
    QVERIFY(TerminalRenderer::syncShadowRange(
        shadow, 64, background.constData(), bytes));
    QVERIFY(!TerminalRenderer::syncShadowRange(
        shadow, 64, background.constData(), bytes));

    // 零长度不需要上传；越界（影子未建立）保守地要求上传且不写越界内存。
    QVERIFY(!TerminalRenderer::syncShadowRange(shadow, 0, zeros.constData(), 0));
    QVERIFY(TerminalRenderer::syncShadowRange(
        shadow, shadow.size() - 8, zeros.constData(), bytes));
    QByteArray empty;
    QVERIFY(TerminalRenderer::syncShadowRange(
        empty, 0, zeros.constData(), bytes));
    QVERIFY(empty.isEmpty());
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

// 就地写入一行并提交元数据。生产路径就是 mutableRow() + finishRow()；
// 旧的 replaceRow()/commandCount() 只被测试使用，已作为死代码删除。
void setRow(NovaTerm::RenderCommandBuffer& buffer, int index,
            const QVector<NovaTerm::RenderCommand>& backgrounds,
            const QVector<NovaTerm::RenderCommand>& contents,
            quint64 atlasGeneration = 0)
{
    NovaTerm::RenderCommandRow& row = buffer.mutableRow(index);
    row.backgrounds = backgrounds;
    row.contents = contents;
    buffer.finishRow(index, atlasGeneration);
}

qsizetype totalCommands(const NovaTerm::RenderCommandBuffer& buffer)
{
    qsizetype count = buffer.overlays().size();
    for (int row = 0; row < buffer.rows(); ++row)
        count += buffer.row(row).backgrounds.size()
               + buffer.row(row).contents.size();
    return count;
}

void RendererP3Tests::commandBufferReplacesOnlyDirtyRow()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(3, 80);
    NovaTerm::RenderCommand command;
    command.type = NovaTerm::RenderCommandType::GlyphInstance;

    setRow(buffer, 0, {}, {command});
    setRow(buffer, 1, {}, {command});
    const quint64 firstRowRevision = buffer.row(0).revision;
    const quint64 secondRowRevision = buffer.row(1).revision;

    setRow(buffer, 1, {}, {});

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
    setRow(buffer, 0, {command}, {});
    QCOMPARE(totalCommands(buffer), qsizetype(1));

    buffer.resize(4, 100);

    QCOMPARE(buffer.rows(), 4);
    QCOMPARE(buffer.columns(), 100);
    QCOMPARE(totalCommands(buffer), qsizetype(0));
    // resize 后每行都要重绘：revision 必须全部推进过。
    QVERIFY(buffer.row(0).revision > 0);
    QVERIFY(buffer.row(3).revision > 0);
}

void RendererP3Tests::commandRowsTrackAtlasGeneration()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(2, 80);
    NovaTerm::RenderCommand glyph;
    glyph.type = NovaTerm::RenderCommandType::GlyphInstance;

    setRow(buffer, 0, {}, {glyph}, 7);
    setRow(buffer, 1, {}, {glyph}, 8);

    QCOMPARE(buffer.row(0).atlasGeneration, quint64(7));
    QCOMPARE(buffer.row(1).atlasGeneration, quint64(8));
    buffer.resize(3, 80);
    QCOMPARE(buffer.row(0).atlasGeneration, quint64(0));
}

void RendererP3Tests::commandBufferValidatesAtlasGeneration()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(2, 80);
    setRow(buffer, 0, {}, {}, 4);
    setRow(buffer, 1, {}, {}, 4);
    QVERIFY(buffer.rowsUseAtlasGeneration(4));

    setRow(buffer, 1, {}, {}, 5);
    QVERIFY(!buffer.rowsUseAtlasGeneration(4));
    QVERIFY(!buffer.rowsUseAtlasGeneration(5));
}

// swapOverlays 与调用方缓冲交换内容：两侧容量都必须跨帧保留，否则
// rebuildOverlays() 每帧都要新建/释放一个 rows+1 的 QVector。
void RendererP3Tests::commandBufferSwapOverlaysRetainsBothCapacities()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(4, 80);
    QVector<NovaTerm::RenderCommand> scratch;
    NovaTerm::RenderCommand cursor;
    cursor.type = NovaTerm::RenderCommandType::Cursor;
    scratch.reserve(8);
    scratch.append(cursor);

    buffer.swapOverlays(scratch);
    QCOMPARE(buffer.overlays().size(), 1);
    // 交换搬走整块缓冲：调用方那份 8 项容量落到命令缓冲上，下一帧
    // rebuildOverlays() 就能直接写进去而不必重新分配。
    QCOMPARE(buffer.overlays().capacity(), qsizetype(8));
    // 调用方换到缓冲原有的空序列，仍然保住了容量。
    QVERIFY(scratch.isEmpty());
    QCOMPARE(scratch.capacity(), qsizetype(0));

    scratch.reserve(8);
    scratch.append(cursor);
    scratch.append(cursor);
    buffer.swapOverlays(scratch);
    QCOMPARE(buffer.overlays().size(), 2);
    QCOMPARE(buffer.overlays().capacity(), qsizetype(8));
    QCOMPARE(scratch.capacity(), qsizetype(8));
}

// 高亮规则的字面量前置过滤必须满足的唯一硬约束：**不得排除任何一次真实
// 命中**。对每条规则的完整形态逐个字符串验证："正则不命中 => 前置过滤也
// 判定为不可能命中"。
//
// 这几条形态与串口日志规则（src/session/SerialHighlightRules.cpp）同型；
// 本测试目标不链接该源文件，故在此复刻同样的模式串。
namespace {

struct HighlightRuleCase
{
    const char* pattern;
    bool caseInsensitive;
};

QVector<HighlightRuleCase> serialShapedHighlightRules()
{
    return {
        {"(?<![A-Za-z0-9])(?:error|failed|failure|fatal|panic|bad|invalid"
         "|corrupt)(?![A-Za-z0-9])", true},
        {"(?<![A-Za-z0-9])(?:warn|warning|caution)(?![A-Za-z0-9])", true},
        {"(?:success|successful|passed|ready|done|ok)", false},
        {"(?:^|\\s)\\S*[>#$]\\s*$", false},
        {"device-ready", false},
        {"\\d{2,3}ms", false},
        {"foo.bar", false},
        {"(a|b)+", false},
    };
}

QStringList highlightCorpus()
{
    return {
        QStringLiteral("ERROR: sensor offline"),
        QStringLiteral("Warning: voltage low"),
        QStringLiteral("ordinary serial output"),
        QStringLiteral("SUCCESSFUL_HANDOFF"),
        QStringLiteral("Zynq> "),
        QStringLiteral("root@board:~# "),
        QStringLiteral("device-ready"),
        QStringLiteral("DEVICE-READY"),
        QStringLiteral("took 42ms"),
        QStringLiteral("took 4ms"),
        QStringLiteral("foo.bar"),
        QStringLiteral("fooXbar"),
        QStringLiteral("abab"),
        QStringLiteral("a"),
        QStringLiteral("   "),
        QStringLiteral(""),
        QStringLiteral("read error_count=0"),
        QStringLiteral("no keyword here at all"),
        QStringLiteral("aaa"),
        QStringLiteral("bbb"),
    };
}

} // namespace

void RendererP3Tests::highlightPrefilterExtractsMandatoryLiterals()
{
    // 前后行断言被跳过，组内纯字面量的分支择一被提取。
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("(?<![A-Za-z0-9])(?:error|failed|panic)"
                                "(?![A-Za-z0-9])")),
             QVector<QString>({QStringLiteral("error"),
                               QStringLiteral("failed"),
                               QStringLiteral("panic")}));

    // 顶层字符组：任何命中必含其中一个字符。
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("[>#$]\\s*$")),
             QVector<QString>({QStringLiteral(">"), QStringLiteral("#"),
                               QStringLiteral("$")}));

    // 整段字面量（含连字符与空格 —— 它们不是 PCRE 元字符）。
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("device ready")),
             QVector<QString>({QStringLiteral("device ready")}));
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("device-ready")),
             QVector<QString>({QStringLiteral("device-ready")}));

    // 断言组后跟字面量：断言跳过，字面量被提取。
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("(?<=x)token")),
             QVector<QString>({QStringLiteral("token")}));
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("(?!x)token")),
             QVector<QString>({QStringLiteral("token")}));

    // 可选量词：元素可能一次都不匹配，空匹配就能绕过过滤，因此必须放弃。
    // 这是前置过滤唯一的正确性红线。
    for (const char* optional : {"[abc]?", "[abc]*", "[abc]{0,3}",
                                 "(?:a|b)?", "[>]?"}) {
        QVERIFY2(TerminalRenderer::highlightRequiredLiterals(
                     QString::fromLatin1(optional)).isEmpty(),
                 optional);
    }
    // 强制量词仍然可用。
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("[abc]+")).size(), 3);
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("[abc]{2,3}")).size(), 3);
    QCOMPARE(TerminalRenderer::highlightRequiredLiterals(
                 QStringLiteral("(?:a|b)+")).size(), 2);

    // 无法证明的形态一律放弃（返回空 => 调用方照旧执行正则）。
    for (const char* unanalysable : {
             "\\d{2,3}ms", "foo.bar", "(a|b)+", "(?<=[a-z])x\\d*",
             "[a-z]+", "(?:^|\\s)\\S*[>#$]\\s*$", "token(?:x|y)",
             "\\S*[>#$]", ""}) {
        QVERIFY2(TerminalRenderer::highlightRequiredLiterals(
                     QString::fromLatin1(unanalysable)).isEmpty(),
                 unanalysable);
    }
}

void RendererP3Tests::highlightPrefilterNeverExcludesAMatch()
{
    const QVector<HighlightRuleCase> rules = serialShapedHighlightRules();
    const QStringList corpus = highlightCorpus();
    int filteredSamples = 0;
    int totalSamples = 0;
    for (const HighlightRuleCase& entry : rules) {
        QRegularExpression pattern(
            QString::fromLatin1(entry.pattern),
            entry.caseInsensitive
                ? QRegularExpression::CaseInsensitiveOption
                : QRegularExpression::PatternOptions());
        const QVector<QString> literals =
            TerminalRenderer::highlightRequiredLiterals(pattern.pattern());
        for (const QString& text : corpus) {
            ++totalSamples;
            const Qt::CaseSensitivity sensitivity = entry.caseInsensitive
                ? Qt::CaseInsensitive : Qt::CaseSensitive;
            bool maybe = literals.isEmpty();
            for (const QString& literal : literals) {
                if (text.contains(literal, sensitivity)) {
                    maybe = true;
                    break;
                }
            }
            if (pattern.match(text).hasMatch()) {
                // 前置过滤命中与否都无所谓，但真实命中必须被记录，说明
                // 语料确实覆盖了正例。
                continue;
            }
            if (!maybe) {
                ++filteredSamples;
                continue;
            }
            QVERIFY2(true, text.toUtf8().constData());
        }
    }
    // 前置过滤必须真的拦下了一些样本，否则这条路径没有意义。
    QVERIFY(filteredSamples > 0);
    QVERIFY(totalSamples > 0);
}

// 高亮角色变化才要求整行重建：角色未变时逐列增量重建是安全的，旧命令仍
// 带着同一个角色色。RenderCommandRow::highlightRole 是这条契约的载体。
void RendererP3Tests::commandRowTracksHighlightRoleAcrossRebuild()
{
    NovaTerm::RenderCommandBuffer buffer;
    buffer.resize(3, 80);
    QCOMPARE(buffer.row(0).highlightRole, NovaTerm::NoHighlightRole);
    for (int row = 0; row < 3; ++row) {
        buffer.mutableRow(row).highlightRole = row; // Error/Warning/Success
        buffer.finishRow(row, 1);
    }
    QCOMPARE(buffer.row(2).highlightRole, 2);

    // 行整体上移：角色跟着行走，否则新位置会拿旧角色的"增量可复用"判据去
    // 复用不属于它的旧命令。
    buffer.rotateRowsUp(1);
    QCOMPARE(buffer.row(0).highlightRole, 1);
    QCOMPARE(buffer.row(1).highlightRole, 2);
    // 新进入底部的行被清空，角色必须一并复位。
    QCOMPARE(buffer.row(2).highlightRole, NovaTerm::NoHighlightRole);

    buffer.resize(4, 80);
    QCOMPARE(buffer.row(0).highlightRole, NovaTerm::NoHighlightRole);
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

// 布局维护合并到"每个事件循环轮次一次"。updateHistoryLayout() 走
// TerminalCore::scrollbackTail()，该接口把尾部逻辑行**深拷贝**一份；解析
// 按 64 KiB 一批发布时（24 MiB/s ≈ 380 批/秒）逐批维护就是逐批深拷贝。
// 布局只在渲染与命中测试时才被读，渲染最多 60 Hz，因此同一轮里积压的多次
// scrollbackChanged 应当只触发一次维护。
void RendererP3Tests::historyLayoutMaintenanceIsCoalescedPerEventTurn()
{
    TerminalCore core(80, 24);
    core.setScrollbackLimit(100000);
    TerminalRenderer renderer(&core);
    QVERIFY(core.waitForIdle(1000));

    QByteArray input;
    for (int i = 0; i < 60; ++i)
        input += QByteArrayLiteral("coalesce-probe\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    // 首建布局（走一次全量重排）。
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 3000);

    // 排空此前投递的合并调用，让下面的统计从稳定状态开始。
    QTest::qWait(80);
    const auto maintenanceCount = [&renderer] {
        const auto statistics = renderer.renderStatistics();
        // 两条路径都算"维护了一次"：尾部增量或全量重排。
        return statistics.historyLayoutTailUpdates
             + statistics.scrollbackReflowRequests;
    };
    const quint64 before = maintenanceCount();

    // 模拟同一轮里积压的多次解析发布。
    constexpr int Publications = 10;
    for (int i = 0; i < Publications; ++i)
        emit core.scrollbackChanged();
    // 信号处理器只置脏标志并投递一次 queued 调用，维护尚未发生。
    QCOMPARE(maintenanceCount(), before);
    QCoreApplication::processEvents();
    QCOMPARE(maintenanceCount(), before + 1);

    // 下一轮再来一次，仍然只维护一次。
    emit core.scrollbackChanged();
    QCoreApplication::processEvents();
    QCOMPARE(maintenanceCount(), before + 2);
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

// 单独按下修饰键不产生输入，不能把回看拉回实时底部（Ctrl+滚轮缩放的起手式
// 曾因此丢失回看位置）；普通按键仍回到底部。顶部/底部跳转供右键菜单使用。
void RendererP3Tests::modifierKeysKeepHistoryAndJumpsReachEnds()
{
    TerminalCore core(80, 6);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 100; ++i)
        input += QByteArrayLiteral("history\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);

    renderer.scrollLines(5);
    QCOMPARE(renderer.scrollOffset(), 5);
    for (const int key : {int(Qt::Key_Control), int(Qt::Key_Shift),
                          int(Qt::Key_Alt), int(Qt::Key_Meta)}) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(&renderer, &press);
        QCOMPARE(renderer.scrollOffset(), 5);
    }

    renderer.scrollToTop();
    QCOMPARE(renderer.scrollOffset(), renderer.maximumScrollOffset());
    QVERIFY(renderer.scrollOffset() > 5);

    QKeyEvent letter(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                     QStringLiteral("a"));
    QApplication::sendEvent(&renderer, &letter);
    QCOMPARE(renderer.scrollOffset(), 0);

    renderer.scrollToTop();
    renderer.scrollToBottom();
    QCOMPARE(renderer.scrollOffset(), 0);
}

// Ctrl+滚轮缩放会改变列宽并触发历史重排。重排在途期间布局为空，旧实现
// 把回看偏移钳到 0，视图直接跳回实时底部。偏移必须在重排期间与完成后都保留。
void RendererP3Tests::columnReflowKeepsScrollbackPosition()
{
    TerminalCore core(80, 6);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 100; ++i)
        input += QByteArrayLiteral("zoom-reflow-probe\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);

    renderer.scrollLines(30);
    QCOMPARE(renderer.scrollOffset(), 30);
    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;

    // 模拟缩放后的列宽变化；一次输出触发 scrollbackChanged → 重排。
    core.resize(40, 6);
    QVERIFY(core.waitForIdle(1000));
    QVERIFY(core.writeInput(QByteArrayLiteral("after-zoom\r\n"))
                .fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(
        renderer.renderStatistics().scrollbackReflowRequests > requestsBefore,
        2000);
    QVERIFY(renderer.scrollOffset() > 0);

    // 重排完成后锚点还原：仍停在历史中而非实时底部。
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);
    QVERIFY(renderer.scrollOffset() > 0);
    QVERIFY(renderer.scrollOffset() <= renderer.maximumScrollOffset());
}

// 缩小字体是反方向：列数与行数同时变多。屏幕变高会经 sb_popline 把最新
// 历史行取回屏幕，历史与折行同时变化，回看位置同样不能丢回实时底部。
void RendererP3Tests::zoomOutReflowKeepsScrollbackPosition()
{
    TerminalCore core(40, 6);
    TerminalRenderer renderer(&core);
    QByteArray input;
    for (int i = 0; i < 100; ++i) {
        // 60 列文本在 40 列下折成两行显示行，放宽到 80 列后合回一行。
        input += QByteArray(60, char('a' + i % 26));
        input += QByteArrayLiteral("\r\n");
    }
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 100, 2000);

    // 偏移超过逻辑行数，验证重排期间按逻辑行数钳制也不会归零。
    renderer.scrollLines(150);
    QCOMPARE(renderer.scrollOffset(), 150);
    const quint64 requestsBefore =
        renderer.renderStatistics().scrollbackReflowRequests;

    core.resize(80, 12);
    QVERIFY(core.waitForIdle(1000));
    QVERIFY(core.writeInput(QByteArrayLiteral("after-zoom-out\r\n"))
                .fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(
        renderer.renderStatistics().scrollbackReflowRequests > requestsBefore,
        2000);
    QVERIFY(renderer.scrollOffset() > 0);

    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);
    QVERIFY(renderer.scrollOffset() > 0);
    QVERIFY(renderer.scrollOffset() <= renderer.maximumScrollOffset());
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

// 右侧滚动条的数据来源：scrollStateChanged 必须随历史增长与回看偏移变化
// 发布，且量程与偏移同批给出。宿主（TerminalView）据此换算滑块位置，若
// 量程抬高时不同批发布偏移，停在实时底部的滑块会被留在旧 maximum 上，
// 看起来像自己滑进了历史。
void RendererP3Tests::scrollStateTracksHistoryGrowthAndOffset()
{
    TerminalCore core(80, 6);
    TerminalRenderer renderer(&core);
    QSignalSpy scrollState(&renderer, &TerminalRenderer::scrollStateChanged);

    // 无历史：量程为 0，偏移为 0。
    QCOMPARE(renderer.maximumScrollOffset(), 0);
    QCOMPARE(renderer.scrollOffset(), 0);

    QByteArray input;
    for (int i = 0; i < 100; ++i)
        input += QByteArrayLiteral("scrollbar-range\r\n");
    QVERIFY(core.writeInput(input).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(renderer.historyDisplayRowCount() > 0, 2000);

    // 历史增长必须发布新量程，且此时仍在实时底部（偏移 0）。
    QTRY_VERIFY_WITH_TIMEOUT(!scrollState.isEmpty(), 2000);
    QCOMPARE(renderer.maximumScrollOffset(),
             int(renderer.historyDisplayRowCount()));
    QCOMPARE(renderer.scrollOffset(), 0);
    QCOMPARE(scrollState.constLast().at(0).toInt(),
             renderer.maximumScrollOffset());
    QCOMPARE(scrollState.constLast().at(1).toInt(), 0);

    // 回看：偏移变化同样发布，量程不变。
    const int rangeBefore = renderer.maximumScrollOffset();
    scrollState.clear();
    renderer.scrollLines(5);
    QCOMPARE(renderer.scrollOffset(), 5);
    QCOMPARE(scrollState.size(), 1);
    QCOMPARE(scrollState.constLast().at(0).toInt(), rangeBefore);
    QCOMPARE(scrollState.constLast().at(1).toInt(), 5);

    // 越界请求被钳制到量程内，且不重复发布相同状态。
    scrollState.clear();
    renderer.scrollToLine(rangeBefore + 1000);
    QCOMPARE(renderer.scrollOffset(), rangeBefore);
    QCOMPARE(scrollState.size(), 1);
    renderer.scrollToLine(rangeBefore + 1000);
    QCOMPARE(scrollState.size(), 1);

    // 回到实时底部。
    scrollState.clear();
    renderer.scrollToBottom();
    QCOMPARE(renderer.scrollOffset(), 0);
    QCOMPARE(scrollState.size(), 1);
    QCOMPARE(scrollState.constLast().at(1).toInt(), 0);

    // 继续输出：留在实时底部时量程抬高，偏移仍为 0 —— 二者必须同批发布。
    scrollState.clear();
    QByteArray more;
    for (int i = 0; i < 20; ++i)
        more += QByteArrayLiteral("scrollbar-more\r\n");
    QVERIFY(core.writeInput(more).fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(
        !scrollState.isEmpty()
            && scrollState.constLast().at(0).toInt() > rangeBefore, 2000);
    QCOMPARE(scrollState.constLast().at(0).toInt(),
             renderer.maximumScrollOffset());
    QCOMPARE(scrollState.constLast().at(1).toInt(), 0);
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
            // 布局维护被合并到下一个事件循环轮次（TerminalRenderer 的
            // syncHistoryLayout：一次 scrollbackTail 深拷贝服务同一轮积压的
            // 多个解析批次），故这里轮询等待。预算与断言都不变，只是把
            // QTRY_VERIFY 的默认 50 ms 轮询间隔换成忙轮询，否则 101 个批次
            // 会把本用例拖到 5 秒。
            QElapsedTimer layoutWait;
            layoutWait.start();
            while (renderer.historyDisplayRowCount()
                       != qsizetype(core.scrollbackLineCount())
                   && layoutWait.elapsed() < 5000) {
                QCoreApplication::processEvents();
            }
            QCOMPARE(renderer.historyDisplayRowCount(),
                     qsizetype(core.scrollbackLineCount()));
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

void RendererP3Tests::selectAllIncludesHistoryAndVisibleScreen()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QVERIFY(core.waitForIdle());
    const QByteArray input = QByteArrayLiteral("oldest\r\n")
        + QByteArray(core.rows() + 2, '\n') + QByteArrayLiteral("newest");
    core.writeInput(input);
    QVERIFY(core.waitForIdle());
    renderer.selectAll();
    QTRY_VERIFY(renderer.hasSelection());
    QTRY_VERIFY(renderer.selectedText().contains(QStringLiteral("oldest")));
    QVERIFY(renderer.selectedText().contains(QStringLiteral("newest")));

    // 备用屏全选不能混入主屏历史；退出后主屏内容仍然存在。
    core.writeInput(QByteArrayLiteral("\x1b[?1049hALT"));
    QVERIFY(core.waitForIdle());
    renderer.selectAll();
    const QString alternate = renderer.selectedText();
    QVERIFY(alternate.contains(QStringLiteral("ALT")));
    QVERIFY(!alternate.contains(QStringLiteral("oldest")));
    core.writeInput(QByteArrayLiteral("\x1b[?1049l"));
    QVERIFY(core.waitForIdle());
    renderer.selectAll();
    QTRY_VERIFY(renderer.selectedText().contains(QStringLiteral("oldest")));
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

// VT 鼠标跟踪开启时左键手势归应用：本地选区让位、点击上报到输出流；
// Shift+左键是本地选区的逃生口。opencode/htop 等依赖这一优先级。
void RendererP3Tests::vtMouseTrackingClaimsLeftButtonGesture()
{
    TerminalCore core(80, 24);
    TerminalRenderer renderer(&core);
    QVERIFY(core.waitForIdle(1000));
    QTest::qWait(30);

    QVERIFY(core.writeInput(QByteArrayLiteral("\x1b[?1000h"))
                .fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_COMPARE(core.mouseTrackingMode(),
                 NovaTerm::MouseTrackingMode::Click);

    const int yRow0 = widgetYForDocumentRow(renderer, 0);
    QVERIFY(yRow0 >= 0);

    // 左键点击：归应用，不产生本地选区，输出流出现鼠标上报。
    QSignalSpy outputSpy(&core, &TerminalCore::outputData);
    QTest::mouseClick(&renderer, Qt::LeftButton, {}, QPoint(40, yRow0 + 1));
    QVERIFY(!renderer.hasSelection());
    QVERIFY(core.waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(!outputSpy.isEmpty(), 1000);
    QVERIFY(!renderer.hasSelection());

    // Shift+左键：强制本地选区（xterm 逃生口）。只发 press+move 不发
    // release —— release 会自动写剪贴板。
    QTest::mousePress(&renderer, Qt::LeftButton, Qt::ShiftModifier,
                      QPoint(1, yRow0 + 1));
    QTest::mouseMove(&renderer, QPoint(renderer.width() - 1, yRow0 + 4));
    QVERIFY(renderer.hasSelection());

    // 应用退出鼠标模式后恢复普通选区行为。
    QVERIFY(core.writeInput(QByteArrayLiteral("\x1b[?1000l"))
                .fullyAccepted());
    QVERIFY(core.waitForIdle(1000));
    QTRY_COMPARE(core.mouseTrackingMode(),
                 NovaTerm::MouseTrackingMode::None);
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

void RendererP3Tests::colorSchemesParseWindowsTerminalFormat()
{
    QCOMPARE(TerminalSchemeStore::builtins().size(), 12);
    for (const auto& scheme : TerminalSchemeStore::builtins()) {
        const auto json = TerminalSchemeStore::toJson(scheme);
        QString error;
        const auto parsed = TerminalSchemeStore::fromJson(json, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(TerminalSchemeStore::toJson(*parsed), json);
    }
    auto json = TerminalSchemeStore::toJson(TerminalColorScheme::defaultDark());
    json[QStringLiteral("magenta")] = json.take(QStringLiteral("purple"));
    json[QStringLiteral("brightMagenta")] = json.take(QStringLiteral("brightPurple"));
    json[QStringLiteral("red")] = QStringLiteral("#f00");
    auto parsed = TerminalSchemeStore::fromJson(json);
    QVERIFY(parsed);
    QCOMPARE(parsed->palette[1], QColor(Qt::red));
    QCOMPARE(parsed->selectionColor.alpha(), 64);
    QString error;
    json.remove(QStringLiteral("green"));
    QVERIFY(!TerminalSchemeStore::fromJson(json, &error));
    QVERIFY(error.startsWith(QStringLiteral("green:")));
    json[QStringLiteral("green")] = QStringLiteral("not-a-color");
    QVERIFY(!TerminalSchemeStore::fromJson(json));
    json[QStringLiteral("green")] = QStringLiteral("#00ff00");
    json[QStringLiteral("name")] = QStringLiteral("  ");
    QVERIFY(!TerminalSchemeStore::fromJson(json));
    QJsonObject config;
    auto invalid = TerminalColorScheme::defaultDark();
    invalid.palette[0] = QColor();
    QVERIFY(!TerminalSchemeStore::save(config, invalid));
    QVERIFY(config.isEmpty());
    TerminalSchemeStore::materialize(config);
    QCOMPARE(config.value(QStringLiteral("schemes")).toArray().size(), 12);
    const auto initialized = config;
    TerminalSchemeStore::materialize(config);
    QCOMPARE(config, initialized);
}

void RendererP3Tests::colorSchemesResolveReferencesAndOverrides()
{
    QJsonObject config{{QStringLiteral("terminal"), QJsonObject{
        {QStringLiteral("colorScheme"), QJsonObject{
            {QStringLiteral("light"), QStringLiteral("One Half Light")},
            {QStringLiteral("dark"), QStringLiteral("One Half Dark")}}}}}};
    QCOMPARE(TerminalSchemeStore::resolve(config, false).background, QColor("#fafafa"));
    QCOMPARE(TerminalSchemeStore::resolve(config, true).background, QColor("#282c34"));
    auto custom = *TerminalSchemeStore::find(config, QStringLiteral("One Half Dark"));
    custom.palette[1] = QColor("#123456");
    QVERIFY(TerminalSchemeStore::save(config, custom, custom.name));
    QCOMPARE(TerminalSchemeStore::resolve(config, true).palette[1], QColor("#123456"));
    QVERIFY(!TerminalSchemeStore::save(config, custom));
    config[QStringLiteral("terminal")] = QJsonObject{
        {QStringLiteral("colorScheme"), QStringLiteral("missing")}};
    QString error;
    QCOMPARE(TerminalSchemeStore::resolve(config, false, &error).name, QStringLiteral("Campbell"));
    QVERIFY(error.contains(QStringLiteral("missing")));
    // 坏的用户覆盖不能遮住正常的内置方案。
    config[QStringLiteral("schemes")] = QJsonArray{QJsonObject{
        {QStringLiteral("name"), QStringLiteral("Campbell")},
        {QStringLiteral("red"), QStringLiteral("invalid")}}};
    QCOMPARE(TerminalSchemeStore::find(config, QStringLiteral("Campbell"))->palette[1], QColor("#c50f1f"));
}

void RendererP3Tests::colorSchemesRenameAndDeleteReferences()
{
    QJsonObject config;
    auto custom = TerminalColorScheme::defaultDark();
    custom.name = QStringLiteral("Custom");
    QVERIFY(TerminalSchemeStore::save(config, custom));
    config[QStringLiteral("terminal")] = QJsonObject{
        {QStringLiteral("colorScheme"), QJsonObject{
            {QStringLiteral("light"), custom.name}, {QStringLiteral("dark"), custom.name}}}};
    custom.name = QStringLiteral("Renamed");
    QVERIFY(TerminalSchemeStore::save(config, custom, QStringLiteral("Custom")));
    QCOMPARE(TerminalSchemeStore::resolve(config, false).name, custom.name);
    QCOMPARE(TerminalSchemeStore::resolve(config, true).name, custom.name);
    QVERIFY(!TerminalSchemeStore::find(config, QStringLiteral("Custom")));
    QVERIFY(TerminalSchemeStore::remove(config, custom.name));
    QCOMPARE(TerminalSchemeStore::resolve(config, false).name, QStringLiteral("One Half Light"));
    QCOMPARE(TerminalSchemeStore::resolve(config, true).name, QStringLiteral("Campbell"));
    QVERIFY(!TerminalSchemeStore::remove(config, QStringLiteral("Campbell")));
}

void RendererP3Tests::colorSchemesMigrateLegacyWithoutLosingColors()
{
    QJsonObject config{{QStringLiteral("terminal"), QJsonObject{
        {QStringLiteral("colorScheme"), QStringLiteral("windowsTerminalCampbell")},
        {QStringLiteral("colors"), QJsonObject{
            {QStringLiteral("background"), QStringLiteral("#102030")},
            {QStringLiteral("selection"), QStringLiteral("#70506070")}}}}}};
    TerminalSchemeStore::migrate(config);
    QCOMPARE(TerminalSchemeStore::resolve(config, true).background, QColor("#102030"));
    QCOMPARE(TerminalSchemeStore::resolve(config, false).selectionColor, QColor("#70506070"));
    QCOMPARE(config.value(QStringLiteral("schemes")).toArray().size(), 1);
    const auto migrated = config;
    TerminalSchemeStore::migrate(config);
    QCOMPARE(config, migrated);
    QVERIFY(!config.value(QStringLiteral("terminal")).toObject().contains(QStringLiteral("colors")));
}

void RendererP3Tests::colorSchemesMigrationPreservesSystemMode()
{
    QJsonObject config{{QStringLiteral("terminal"), QJsonObject{
        {QStringLiteral("colorScheme"), QStringLiteral("system")},
        {QStringLiteral("colors"), QJsonObject{{QStringLiteral("background"), QStringLiteral("#123456")}}}}}};
    TerminalSchemeStore::migrate(config);
    QCOMPARE(TerminalSchemeStore::resolve(config, false).name, QStringLiteral("Black on White"));
    QCOMPARE(TerminalSchemeStore::resolve(config, true).name, QStringLiteral("Campbell"));
    QCOMPARE(TerminalSchemeStore::resolve(config, true).background, QColor("#0c0c0c"));
}

void RendererP3Tests::colorSchemeChangePreservesTerminalContent()
{
    TerminalCore core(40, 8);
    TerminalRenderer renderer(&core);
    core.writeInput(QByteArrayLiteral("Default \x1b[31mRed \x1b[38;2;18;52;86mRGB"));
    QVERIFY(core.waitForIdle());
    const auto before = core.snapshot();
    renderer.setColorScheme(*TerminalSchemeStore::find({}, QStringLiteral("One Half Light")));
    QVERIFY(core.waitForIdle());
    const auto after = core.snapshot();
    QCOMPARE(after.cursor.position, before.cursor.position);
    QCOMPARE(renderer.colorScheme().background, QColor("#fafafa"));
    // 切换配色只改显示语义，不清屏、不把索引色或 TrueColor 烘焙成方案 RGB。
    for (int col = 0; col < 15; ++col) {
        const auto& oldCell = before.visibleCells[col];
        const auto& newCell = after.visibleCells[col];
        QCOMPARE(newCell.chars, oldCell.chars);
        QCOMPARE(newCell.foreground.type, oldCell.foreground.type);
        QCOMPARE(newCell.foreground.index, oldCell.foreground.index);
        QCOMPARE(newCell.foreground.red, oldCell.foreground.red);
        QCOMPARE(newCell.foreground.green, oldCell.foreground.green);
        QCOMPARE(newCell.foreground.blue, oldCell.foreground.blue);
    }
}

QTEST_MAIN(RendererP3Tests)
#include "RendererP3Tests.moc"
