#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"
#include "ui/terminal/TerminalView.h"

#include <ElaComboBox.h>

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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
    void conPtyStartupKeepsUiResponsive();
    void terminalViewStartupKeepsUiResponsive();
    void terminalViewRepeatedStartStop();
    void comboBoxAnimationTeardownIsSafe();
};

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

QTEST_MAIN(TerminalSessionSmokeTests)
#include "TerminalSessionSmokeTests.moc"
