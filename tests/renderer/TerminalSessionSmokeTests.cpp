#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"
#include "session/TerminalSession.h"
#include "ui/terminal/TerminalView.h"
#include "service/ConfigManager.h"
#include "service/TerminalSchemeStore.h"

#include <ElaComboBox.h>
#include <ElaScrollBar.h>
#include <ElaTheme.h>

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLayout>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

namespace {

class TestComboBox final : public ElaComboBox
{
public:
    using ElaComboBox::ElaComboBox;
    using ElaComboBox::hidePopup;
    using ElaComboBox::showPopup;
};

} // namespace

class TerminalSessionSmokeTests : public QObject
{
    Q_OBJECT

private slots:
    void terminalColorsAreIndependentOfApplicationTheme();
    void savedSchemeRepaintsExistingTerminalPixels();
    void conPtyStartupKeepsUiResponsive();
    void terminalViewStartupKeepsUiResponsive();
    void terminalViewRepeatedStartStop();
    void terminalViewStartupPreservesPendingSize();
    void terminalViewScrollBarDrivesHistoryScrollback();
    void comboBoxAnimationTeardownIsSafe();
    void externalSessionOutlivesView();
    void ownedDependenciesAreDestroyedBeforeCore();
};

void TerminalSessionSmokeTests::savedSchemeRepaintsExistingTerminalPixels()
{
    const auto previous = ConfigManager::instance().root();
    QVERIFY(ConfigManager::setValues({
        {QStringLiteral("terminal.appearance"), QStringLiteral("dark")},
        {QStringLiteral("terminal.colorScheme"), QStringLiteral("Campbell")}}));
    TerminalView first;
    TerminalView second;
    first.resize(640, 320);
    first.show();
    QVERIFY(QTest::qWaitForWindowExposed(&first, 3000));
    auto* core = first.session()->core();
    auto* renderer = first.renderer();
    core->writeInput(QByteArrayLiteral(
        "\x1b[41m        \x1b[48;2;18;52;86m        \x1b[0m\x1b[?25l"));
    QVERIFY(core->waitForIdle());
    const auto cursor = core->snapshot().cursor.position;
    auto* session = first.session();
    auto hasColor = [renderer](const QColor& expected) {
        const QImage image = renderer->grabFramebuffer();
        if (image.isNull())
            return false;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).rgb() == expected.rgb())
                    return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasColor(QColor("#c50f1f")), 5000);
    QVERIFY(hasColor(QColor("#123456")));

    auto config = ConfigManager::instance().root();
    auto edited = *TerminalSchemeStore::find(config, QStringLiteral("Campbell"));
    edited.palette[1] = QColor("#12ab34");
    edited.background = QColor("#172b41");
    QVERIFY(TerminalSchemeStore::save(config, edited, edited.name));
    QVERIFY(ConfigManager::setValues({
        {QStringLiteral("schemes"), config.value(QStringLiteral("schemes")).toVariant()}}));
    QTRY_COMPARE(second.renderer()->colorScheme().palette[1], edited.palette[1]);
    // 不再喂入任何字节，直接验证原有内容、ANSI 背景和 GPU 清屏色立即更新。
    QTRY_VERIFY_WITH_TIMEOUT(hasColor(edited.palette[1]), 5000);
    QVERIFY(hasColor(edited.background));
    QVERIFY(hasColor(QColor("#123456")));
    QVERIFY(core->waitForIdle());
    QCOMPARE(first.session(), session);
    QCOMPARE(core->snapshot().cursor.position, cursor);
    ConfigManager::setValues(previous.toVariantMap());
}

void TerminalSessionSmokeTests::terminalColorsAreIndependentOfApplicationTheme()
{
    // 不调用 load()，测试只操作内存，不覆盖真实配置文件。
    const auto previous = ConfigManager::instance().root().value(QStringLiteral("terminal"));
    const auto originalTheme = eTheme->getThemeMode();
    const QJsonObject references{
        {QStringLiteral("dark"), QStringLiteral("Dracula")},
        {QStringLiteral("light"), QStringLiteral("One Half Light")}
    };
    ConfigManager::setValues({
        {QStringLiteral("terminal.appearance"), QStringLiteral("dark")},
        {QStringLiteral("terminal.colorScheme"), references.toVariantMap()}});
    TerminalView first;
    TerminalView second;
    auto* core = first.session()->core();
    core->writeInput(QByteArrayLiteral("persistent output"));
    QVERIFY(core->waitForIdle());
    const auto* session = first.session();
    QCOMPARE(first.renderer()->colorScheme().name, QStringLiteral("Dracula"));
    eTheme->setThemeMode(ElaThemeType::Light);
    QApplication::processEvents();
    QCOMPARE(first.renderer()->colorScheme().name, QStringLiteral("Dracula"));
    ConfigManager::set(QStringLiteral("terminal.appearance"), QStringLiteral("light"));
    QTRY_COMPARE(first.renderer()->colorScheme().name, QStringLiteral("One Half Light"));
    QTRY_COMPARE(second.renderer()->colorScheme().name, QStringLiteral("One Half Light"));
    QCOMPARE(first.session(), session);
    QVERIFY(core->waitForIdle());
    QCOMPARE(core->snapshot().visibleCells[0].chars[0], uint32_t('p'));
    eTheme->setThemeMode(ElaThemeType::Dark);
    QApplication::processEvents();
    QCOMPARE(first.renderer()->colorScheme().name, QStringLiteral("One Half Light"));
    ConfigManager::set(QStringLiteral("terminal"), previous.toVariant());
    eTheme->setThemeMode(originalTheme);
}

void TerminalSessionSmokeTests::terminalViewStartupPreservesPendingSize()
{
    TerminalView view;
    view.layout()->activate();
    auto* renderer = view.findChild<TerminalRenderer*>();
    QVERIFY(renderer);
    auto* core = view.findChild<TerminalCore*>();
    QVERIFY(core);
    QVERIFY(core->waitForIdle());
    const int columns = core->columns() + 20;
    const int rows = core->rows() + 10;
    // 模拟 renderer 已发布目标、Parser 尚未处理 resize 的确定性窗口。
    renderer->terminalSizeChanged(columns, rows);

    LocalShellConfig config;
    config.profile.name = QStringLiteral("pending size probe");
    config.profile.executable = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("novaterm_conpty_test_child.exe"));
    config.profile.arguments = {QStringLiteral("size")};
    view.startLocalShell(config);
    auto* transport = view.findChild<LocalShellTransport*>();
    QVERIFY(transport);
    QByteArray output;
    connect(transport, &ITransport::readyRead, this,
            [&output](const QByteArray& bytes) { output += bytes; });
    const QByteArray expected = "SIZE=" + QByteArray::number(columns)
        + "x" + QByteArray::number(rows);
    QTRY_VERIFY_WITH_TIMEOUT(output.contains(expected), 5000);
    view.stopLocalShell();
}

// 终端右侧滚动条：渲染器是唯一滚动状态来源，滚动条既要跟随它（滚轮、
// 历史增长），也要能反向驱动它（拖动滑块回看历史）。坐标方向相反 ——
// 滚动条底部 = 实时底部 = 渲染器偏移 0。
void TerminalSessionSmokeTests::terminalViewScrollBarDrivesHistoryScrollback()
{
    TerminalView view;
    view.resize(640, 320);
    view.layout()->activate();
    auto* bar = view.scrollBar();
    QVERIFY(bar);
    auto* renderer = view.renderer();
    auto* core = view.session()->core();

    // 无历史：量程为 0，滑块停在底部。
    QCOMPARE(bar->maximum(), 0);
    QCOMPARE(bar->value(), 0);

    QByteArray input;
    for (int i = 0; i < 100; ++i)
        input += QByteArrayLiteral("scrollbar-view\r\n");
    QVERIFY(core->writeInput(input).fullyAccepted());
    QVERIFY(core->waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(bar->maximum() > 0, 3000);

    // 有历史但仍在实时底部：滑块必须停在 maximum，而不是被留在旧量程上。
    QCOMPARE(bar->maximum(), renderer->maximumScrollOffset());
    QCOMPARE(renderer->scrollOffset(), 0);
    QCOMPARE(bar->value(), bar->maximum());
    // 页步取可见行数，使滑块长度反映视口占比。
    QCOMPARE(bar->pageStep(), core->rows());

    // 渲染器 → 滚动条：滚轮回看后滑块相应上移。
    renderer->scrollLines(5);
    QCOMPARE(renderer->scrollOffset(), 5);
    QCOMPARE(bar->value(), bar->maximum() - 5);

    // 滚动条 → 渲染器：拖动滑块到顶端即回看到最旧历史。
    bar->setValue(0);
    QCOMPARE(renderer->scrollOffset(), renderer->maximumScrollOffset());
    // 反向驱动不得形成回环：渲染器发布的新状态换算回同一个值。
    QCOMPARE(bar->value(), 0);

    // 拖回底部即回到实时输出。
    bar->setValue(bar->maximum());
    QCOMPARE(renderer->scrollOffset(), 0);

    // 停在实时底部时继续输出：量程抬高，滑块跟着走到新的底部。
    const int rangeBefore = bar->maximum();
    QByteArray more;
    for (int i = 0; i < 20; ++i)
        more += QByteArrayLiteral("scrollbar-view-more\r\n");
    QVERIFY(core->writeInput(more).fullyAccepted());
    QVERIFY(core->waitForIdle(1000));
    QTRY_VERIFY_WITH_TIMEOUT(bar->maximum() > rangeBefore, 3000);
    QCOMPARE(renderer->scrollOffset(), 0);
    QCOMPARE(bar->value(), bar->maximum());
}

void TerminalSessionSmokeTests::conPtyStartupKeepsUiResponsive()
{
    TerminalCore core(100, 30);
    TerminalRenderer renderer(&core);
    renderer.resize(1000, 600);
    renderer.show();
    QVERIFY(QTest::qWaitForWindowExposed(&renderer, 3000));

    LocalShellTransport transport;
    transport.setShellProgram(QStringLiteral("cmd.exe"));
    const QString clinkBat = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("clink.bat"));
    QVERIFY2(QFileInfo::exists(clinkBat), qPrintable(clinkBat));
    transport.setShellArgs({
        QStringLiteral("/k"),
        QDir::toNativeSeparators(clinkBat),
        QStringLiteral("inject")
    });
    transport.resizeTerminal(100, 30);

    connect(&transport, &ITransport::readyRead, &core,
            [&core](const QByteArray& data) {
        if (!data.isEmpty())
            core.writeInput(data);
    });
    connect(&core, &TerminalCore::outputData, &transport,
            [&transport](const QByteArray& data) {
        if (transport.isConnected())
            transport.write(data);
    });

    QVERIFY2(transport.connectToHost(),
             qPrintable(transport.errorString()));

    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this,
            [&heartbeatCount]() { ++heartbeatCount; });
    heartbeat.start();

    core.setScrollbackLimit(0);
    QTest::qWait(250);
    transport.write(QByteArrayLiteral(
        "for /L %i in (1,1,2000) do @echo NovaTerm-P3-%i\r\n"));
    QTimer::singleShot(1500, &core,
                       [&core]() { core.setScrollbackLimit(1000); });
    QTest::qWait(2500);

    const auto renderStats = renderer.renderStatistics();
    const auto queueStats = core.queueStatistics();
    qInfo() << "session smoke stats:"
            << "heartbeats" << heartbeatCount
            << "frames" << renderStats.scheduler.framesRequested
            << "fullFrames" << renderStats.scheduler.fullFrames
            << "rowsRebuilt" << renderStats.rowsRebuilt
            << "uploadBytes" << renderStats.gpuUploadBytes
            << "queuedBytes" << queueStats.queuedBytes;

    QVERIFY2(heartbeatCount >= 100,
             "UI event loop was starved during ConPTY startup/output");
    QVERIFY2(renderStats.scheduler.framesRequested > 0,
             "No scheduled render frame was observed");
    QVERIFY2(core.waitForIdle(5000),
             "Parser queue did not drain after terminal output");

    transport.disconnect();
}

void TerminalSessionSmokeTests::terminalViewStartupKeepsUiResponsive()
{
    TerminalView view;
    view.resize(1000, 600);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view, 3000));

    int heartbeatCount = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this,
            [&heartbeatCount]() { ++heartbeatCount; });
    heartbeat.start();

    view.startLocalShell(TerminalView::LocalShellType::Cmd);
    QVERIFY(view.isLocalShell());
    QTest::qWait(3000);

    QVERIFY2(heartbeatCount >= 100,
             "TerminalView startup starved the UI event loop");
    QVERIFY(view.renderer()->renderStatistics().scheduler.framesRequested > 0);
    view.stopLocalShell();
}

void TerminalSessionSmokeTests::terminalViewRepeatedStartStop()
{
    TerminalView view;
    LocalShellConfig config;
    config.profile.name = QStringLiteral("TerminalView lifecycle child");
    config.profile.executable = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("novaterm_conpty_test_child.exe"));
    config.profile.arguments = {QStringLiteral("hold")};
    QVERIFY(QFileInfo::exists(config.profile.executable));

    QSignalSpy finished(&view, &TerminalView::shellFinished);
    for (int iteration = 0; iteration < 200; ++iteration) {
        finished.clear();
        view.startLocalShell(config);
        QTRY_VERIFY_WITH_TIMEOUT(view.transport() != nullptr, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(view.transport()
                                     && view.transport()->isConnected(),
                                 5000);
        view.stopLocalShell();
        if (finished.isEmpty())
            QVERIFY2(finished.wait(5000), qPrintable(QString::number(iteration)));
        QCOMPARE(finished.size(), 1);
        QVERIFY(!view.isLocalShell());
    }
}

void TerminalSessionSmokeTests::comboBoxAnimationTeardownIsSafe()
{
    for (int iteration = 0; iteration < 200; ++iteration) {
        auto* owner = new QWidget;
        auto* comboBox = new TestComboBox(owner);
        comboBox->addItems({QStringLiteral("cmd"),
                            QStringLiteral("PowerShell")});
        owner->show();
        comboBox->showPopup();
        QCoreApplication::processEvents();
        comboBox->hidePopup();

        // Session selection destroys the dialog while the popup animations
        // may still be running. Exercise that exact QObject/style teardown.
        delete owner;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

void TerminalSessionSmokeTests::externalSessionOutlivesView()
{
    RuntimeConfig config;
    config.title = QStringLiteral("background session");
    TerminalSession session(config);
    {
        TerminalView firstView(&session);
        TerminalView secondView(&session);
        QVERIFY(!firstView.ownsSession());
        QVERIFY(!secondView.ownsSession());
        QCOMPARE(firstView.session(), &session);
        QCOMPARE(secondView.session(), &session);
    }
    QCOMPARE(session.state(), SessionState::Created);
}

void TerminalSessionSmokeTests::ownedDependenciesAreDestroyedBeforeCore()
{
    auto* view = new TerminalView;
    auto* core = view->session()->core();
    auto* renderer = view->renderer();
    auto* session = view->session();
    QStringList destructionOrder;
    connect(renderer, &QObject::destroyed, this,
            [&destructionOrder] { destructionOrder.append(QStringLiteral("renderer")); });
    connect(session, &QObject::destroyed, this,
            [&destructionOrder] { destructionOrder.append(QStringLiteral("session")); });
    connect(core, &QObject::destroyed, this,
            [&destructionOrder] { destructionOrder.append(QStringLiteral("core")); });

    delete view;

    QCOMPARE(destructionOrder,
             QStringList({QStringLiteral("renderer"),
                          QStringLiteral("session"),
                          QStringLiteral("core")}));
}

QTEST_MAIN(TerminalSessionSmokeTests)
#include "TerminalSessionSmokeTests.moc"
