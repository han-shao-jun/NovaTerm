#include "ui/widgets/SftpPanel.h"
#include "ui/widgets/SshPanelStack.h"
#include "ui/widgets/SystemMonitorPanel.h"
#include "ui/widgets/SystemInformationDialog.h"
#include "session/SftpSession.h"
#include "service/ResourcePrefetch.h"
#include "transport/SshTransport.h"
#include "ElaApplication.h"
#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaProgressBar.h"
#include <QApplication>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>
#include <QLabel>
#include <memory>

// Only the network worker is replaced. Panels, queues, parsing and timers are real.
class SftpSessionSafetyTestAccess
{
public:
    static void connected(SftpSession& session, const QString& home)
    {
        session._control->running = true;
        emit session.connected(home);
    }
    static quint64 generation(SftpSession& session)
    {
        return session._control->generation;
    }
    static bool running(SftpSession& session)
    {
        return session._control->running;
    }
    static void drainQueue(SftpSession& session)
    {
        QMutexLocker lock(&session._control->queueMutex);
        session._control->commands.clear();
    }
    static void available(SftpSession& session) { session._control->connected = true; }
    static qsizetype queued(SftpSession& session) {
        QMutexLocker lock(&session._control->queueMutex);
        return session._control->commands.size();
    }
    static SftpFileTransferDirection queuedDirection(SftpSession& session) {
        QMutexLocker lock(&session._control->queueMutex);
        return session._control->commands.head().fileUpload.direction;
    }
    static void active(SftpSession& session, bool active)
    {
        QMutexLocker lock(&session._control->queueMutex);
        session._control->commandActive = active;
    }
    static bool busyWhileLocked(SftpSession& session)
    {
        QMutexLocker lock(&session._control->queueMutex);
        return session.hasPendingOperations();
    }
};

class SshTransportTestAccess
{
public:
    static void connected(SshTransport& transport)
    {
        transport._connected = true;
    }
    static void verified(SshTransport& transport) {
        connected(transport); transport._serverHostKeyFingerprint = QString(64, 'a');
    }
    static void advance(SshTransport& transport) { ++transport._connectionGeneration; }
    static bool monitoring(SshTransport& transport) { return transport._monitorEnabled; }
    static quint64 generation(SshTransport& transport) { return transport._monitorGeneration; }
    static quint64 pendingSample(SshTransport& transport) { return transport._monitorRequestId; }
    static bool includesFileSystems(SshTransport& transport) { return transport._monitorIncludeFileSystems; }
    static bool commandPending(SshTransport& transport) { return !transport._commandQueue.isEmpty(); }
    static QByteArray finishCommand(SshTransport& transport, const QByteArray& output)
    {
        if (transport._commandQueue.isEmpty())
            return {};
        const auto command = transport._commandQueue.dequeue();
        emit transport.commandFinished(command.requestId, output, {}, {});
        return command.command;
    }
    static void finishSample(SshTransport& transport, const QByteArray& output,
                             const QString& error = {})
    {
        const quint64 id = std::exchange(transport._monitorRequestId, 0);
        emit transport.resourceSampleFinished(id, output, error);
    }
};

class TerminalDirectoryResults final : public QObject
{
    Q_OBJECT
signals:
    void resolved(SshTransport* source, const QString& path);
    void failed(SshTransport* source);
};

class SftpPanelTests final : public QObject
{
    Q_OBJECT
private:
    static QByteArray sample()
    {
        return QByteArrayLiteral("@@stat\ncpu 100 0 50 200 0 0 0 0\n"
            "@@meminfo\nMemTotal: 1000 kB\nMemAvailable: 500 kB\n"
            "SwapTotal: 100 kB\nSwapFree: 25 kB\n@@net/dev\n"
            "eth0: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n");
    }
    static void finishInitialBaseline(SshTransport& transport)
    {
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(transport) != 0);
        SshTransportTestAccess::finishSample(transport, sample());
    }
    static void periodicTick(SystemMonitorPanel& panel)
    {
        auto* timer = panel.findChild<QTimer*>(QString{}, Qt::FindDirectChildrenOnly);
        QVERIFY(timer);
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    }
private slots:
    void authorizedUploadReusesPanelQueueAndBlocksCompetingUiRequests_data()
    {
        QTest::addColumn<bool>("download");
        QTest::newRow("upload") << false;
        QTest::newRow("download") << true;
    }
    void authorizedUploadReusesPanelQueueAndBlocksCompetingUiRequests()
    {
        QFETCH(bool, download);
        SshTransport transport(SshConfig{});
        SshTransportTestAccess::verified(transport);
        SftpPanel panel;
        panel.setSessionContext("Board", &transport);
        auto* backend = panel.findChild<SftpSession*>(); QVERIFY(backend);
        SftpSessionSafetyTestAccess::connected(*backend, "/board");
        SftpSessionSafetyTestAccess::available(*backend);
        SftpSessionSafetyTestAccess::drainQueue(*backend);
        emit backend->directoryListed("/board", {});
        QCOMPARE(panel.fileTransferSession(&transport), backend);
        SftpFileUploadRequest request;
        request.requestId = 42; request.localRoot = "C:/build"; request.localRelativePath = "module.ko";
        request.remoteRoot = "/board"; request.remoteName = "module-build.ko";
        request.control = std::make_shared<SftpFileUploadControl>();
        request.direction = download ? SftpFileTransferDirection::Download : SftpFileTransferDirection::Upload;
        QVERIFY(panel.uploadAuthorizedFile(request));
        QCOMPARE(SftpSessionSafetyTestAccess::queued(*backend), 1);
        QCOMPARE(SftpSessionSafetyTestAccess::queuedDirection(*backend), request.direction);
        QVERIFY(panel.hasPendingOperations());
        QVERIFY(!panel.hasPendingUserOperations());
        QVERIFY(!panel.findChild<ElaLineEdit*>()->isEnabled());
        QVERIFY(!panel.uploadAuthorizedFile(request));
        panel.synchronizePathFromTerminal(&transport, "/competing");
        QCOMPARE(SftpSessionSafetyTestAccess::queued(*backend), 1);
        SftpFileUploadResult result; result.requestId = 41;
        emit backend->authorizedFileFinished(result);
        QVERIFY(!panel.findChild<ElaLineEdit*>()->isEnabled());
        result.requestId = 42;
        SftpSessionSafetyTestAccess::drainQueue(*backend);
        emit backend->authorizedFileFinished(result);
        QVERIFY(panel.findChild<ElaLineEdit*>()->isEnabled());
        QCOMPARE(panel.fileTransferSession(&transport), backend);
        SshTransportTestAccess::advance(transport);
        QVERIFY(!panel.fileTransferSession(&transport));
    }
    void closeSafetySeesBackgroundPanelOperations()
    {
        SshTransport a(SshConfig{}), b(SshConfig{});
        SshTransportTestAccess::connected(a); SshTransportTestAccess::connected(b);
        SshPanelStack<SftpPanel> stack;
        QVERIFY(!stack.panelFor(&a));
        stack.setSessionContext("A", &a);
        auto* panel = stack.panelFor(&a); QVERIFY(panel);
        QVERIFY(panel->hasPendingOperations());
        auto* backend = panel->findChild<SftpSession*>();
        SftpSessionSafetyTestAccess::connected(*backend, "/a");
        SftpSessionSafetyTestAccess::drainQueue(*backend);
        emit backend->directoryListed("/a", {});
        QVERIFY(!panel->hasPendingOperations());
        panel->synchronizePathFromTerminal(&a, "/next");
        QVERIFY(panel->hasPendingOperations());
        stack.setSessionContext("B", &b);
        QCOMPARE(stack.panelFor(&a), panel);
        QVERIFY(panel->hasPendingOperations());
        // A delayed list completion clears the UI flag, but not the queued worker operation.
        emit backend->directoryListed("/a", {});
        QVERIFY(panel->hasPendingOperations());
        SftpSessionSafetyTestAccess::drainQueue(*backend);
        emit backend->directoryListed("/next", {});
        QVERIFY(!panel->hasPendingOperations());
        SftpSessionSafetyTestAccess::active(*backend, true);
        QVERIFY(panel->hasPendingOperations());
        SftpSessionSafetyTestAccess::active(*backend, false);
        QVERIFY(!panel->hasPendingOperations());
        QVERIFY(SftpSessionSafetyTestAccess::busyWhileLocked(*backend));
    }
    void backgroundDirectoryQueryFinishesOnItsOriginPanel_data()
    {
        QTest::addColumn<bool>("success");
        QTest::newRow("success") << true;
        QTest::newRow("timeout") << false;
    }
    void backgroundDirectoryQueryFinishesOnItsOriginPanel()
    {
        QFETCH(bool, success);
        SshTransport a(SshConfig{}), b(SshConfig{});
        SshTransportTestAccess::connected(a); SshTransportTestAccess::connected(b);
        SshPanelStack<SftpPanel> stack; TerminalDirectoryResults results;
        const auto prepare = [&](SshTransport& transport, const QString& label, const QString& path) {
            stack.setSessionContext(label, &transport); auto* panel = stack.currentPanel();
            auto* backend = panel->findChild<SftpSession*>();
            SftpSessionSafetyTestAccess::connected(*backend, path); emit backend->directoryListed(path, {});
            return panel;
        };
        auto* panelA = prepare(a, "A", "/a");
        QVERIFY(connect(&results, SIGNAL(resolved(SshTransport*,QString)), panelA,
                        SLOT(synchronizePathFromTerminal(SshTransport*,QString))));
        QVERIFY(connect(&results, SIGNAL(failed(SshTransport*)), panelA, SLOT(terminalPathLookupFailed(SshTransport*))));
        const auto begin = [](SftpPanel* panel) {
            for (auto* button : panel->findChildren<ElaIconButton*>())
                if (button->accessibleName() == "Synchronize SFTP path") { button->click(); return true; }
            return false;
        };
        QVERIFY(begin(panelA)); QVERIFY(!panelA->findChild<ElaLineEdit*>()->isEnabled());
        auto* panelB = prepare(b, "B", "/b");
        QVERIFY(connect(&results, SIGNAL(resolved(SshTransport*,QString)), panelB,
                        SLOT(synchronizePathFromTerminal(SshTransport*,QString))));
        QVERIFY(connect(&results, SIGNAL(failed(SshTransport*)), panelB, SLOT(terminalPathLookupFailed(SshTransport*))));
        QVERIFY(begin(panelB)); QVERIFY(!panelB->findChild<ElaLineEdit*>()->isEnabled());
        emit results.failed(nullptr); QVERIFY(!panelA->findChild<ElaLineEdit*>()->isEnabled());
        if (success) {
            emit results.resolved(&a, "/from-a");
            emit panelA->findChild<SftpSession*>()->directoryListed("/from-a", {});
        } else emit results.failed(&a);
        QVERIFY(panelA->findChild<ElaLineEdit*>()->isEnabled());
        QVERIFY(!panelB->findChild<ElaLineEdit*>()->isEnabled());
        QCOMPARE(panelA->findChild<ElaLineEdit*>()->text(), success ? QString("/from-a") : QString("/a"));
        QCOMPARE(panelB->findChild<ElaLineEdit*>()->text(), QString("/b"));
        stack.setSessionContext("A", &a); QVERIFY(panelA->findChild<ElaLineEdit*>()->isEnabled());
        emit results.failed(&b); QVERIFY(panelB->findChild<ElaLineEdit*>()->isEnabled());
    }
    void detailScheduleRetainsOriginalDeferral()
    {
        NovaTerm::LinuxResource::PrefetchSchedule schedule;
        QVERIFY(!schedule.ready(0));
        QVERIFY(!schedule.ready(1199));
        QVERIFY(schedule.ready(1200));
        schedule.started(1200);
        schedule.finished(1240);
        QVERIFY(!schedule.ready(2199));
        QVERIFY(schedule.ready(2200));
        schedule.started(2200);
        schedule.finished(5000);
        QVERIFY(!schedule.ready(5249));
        QVERIFY(schedule.ready(5250));
    }

    void firstMetricsDoNotWaitForStaticDetails()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        QVERIFY(!SshTransportTestAccess::commandPending(a));
        SshTransportTestAccess::finishSample(a, sample());
        QCOMPARE(panel.findChildren<ElaProgressBar*>()[1]->value(), 50);
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        QVERIFY(!SshTransportTestAccess::commandPending(a));
    }

    void failedFirstSampleRetriesBeforeStartingStaticQueries()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        SshTransportTestAccess::finishSample(a, {}, QStringLiteral("temporary failure"));
        QTest::qWait(100);
        QVERIFY(!SshTransportTestAccess::commandPending(a));
        QCOMPARE(SshTransportTestAccess::pendingSample(a), quint64{0});
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 1500);
        SshTransportTestAccess::finishSample(a, sample());
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
    }

    void steadySamplingDoesNotRunImmediatelyAfterCompletion()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        SshTransportTestAccess::finishSample(a, sample());
        QTest::qWait(100);
        QCOMPARE(SshTransportTestAccess::pendingSample(a), quint64{0});
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 1500);
    }

    void initialDisplayUsesProgressiveValuesWithoutCollectingText()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        panel.findChild<ElaIconButton*>()->click();
        for (const auto* label : panel.findChildren<QLabel*>())
            QVERIFY2(!label->text().contains(QStringLiteral("Collecting")), qPrintable(label->text()));
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        SshTransportTestAccess::finishSample(a, sample()
            + QByteArrayLiteral("@@filesystems\n/dev/root 1000 500 500 50% /\n"));
        QCOMPARE(panel.findChildren<ElaProgressBar*>()[1]->value(), 50);
        QCOMPARE(panel.findChildren<ElaProgressBar*>()[0]->format(), QStringLiteral("—"));
        for (const auto* label : panel.findChildren<QLabel*>())
            QVERIFY2(!label->text().contains(QStringLiteral("Collecting")), qPrintable(label->text()));
    }

    void filesystemRefreshRetainsTenSecondCadence()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        panel.findChild<ElaIconButton*>()->click();
        QCoreApplication::processEvents();
        SshTransportTestAccess::finishSample(a, sample());
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        QVERIFY(SshTransportTestAccess::includesFileSystems(a));
        SshTransportTestAccess::finishSample(a, sample()
            + QByteArrayLiteral("@@filesystems\n/dev/root 1000 500 500 50% /\n"));
        QElapsedTimer elapsed;
        elapsed.start();
        bool refreshed = false;
        while (elapsed.elapsed() < 12000) {
            QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 1500);
            if (SshTransportTestAccess::includesFileSystems(a)) {
                QVERIFY(elapsed.elapsed() >= 9900);
                refreshed = true;
                break;
            }
            SshTransportTestAccess::finishSample(a, sample());
        }
        QVERIFY(refreshed);
    }

    void switchingSessionsKeepsConnectionsAndDirectories()
    {
        SshTransport a(SshConfig{}), b(SshConfig{});
        SshPanelStack<SftpPanel> stack;
        stack.setSessionContext(QStringLiteral("same label"), &a);
        auto* panelA = stack.currentPanel();
        auto* backendA = panelA->findChild<SftpSession*>();
        SftpSessionSafetyTestAccess::connected(*backendA, QStringLiteral("/home/a"));
        emit backendA->directoryListed(QStringLiteral("/srv/a"), {});
        const auto generation = SftpSessionSafetyTestAccess::generation(*backendA);

        stack.setSessionContext(QStringLiteral("same label"), &b);
        auto* panelB = stack.currentPanel();
        auto* backendB = panelB->findChild<SftpSession*>();
        QVERIFY(panelA != panelB);
        SftpSessionSafetyTestAccess::connected(*backendB, QStringLiteral("/home/b"));
        emit backendB->directoryListed(QStringLiteral("/var/log/b"), {});
        QVERIFY(SftpSessionSafetyTestAccess::running(*backendA));
        QCOMPARE(SftpSessionSafetyTestAccess::generation(*backendA), generation);
        // Background results update A only, even while B is selected.
        emit backendA->directoryListed(QStringLiteral("/srv/a/subdir"), {});
        QCOMPARE(panelB->findChild<ElaLineEdit*>()->text(), QStringLiteral("/var/log/b"));

        stack.setSessionContext(QStringLiteral("renamed A"), &a);
        QCOMPARE(stack.currentPanel(), panelA);
        QCOMPARE(stack.windowTitle(), QStringLiteral("renamed A"));
        QCOMPARE(panelA->findChild<ElaLineEdit*>()->text(), QStringLiteral("/srv/a/subdir"));
        QCOMPARE(SftpSessionSafetyTestAccess::generation(*backendA), generation);
        stack.setSessionContext(QStringLiteral("local shell"), nullptr);
        QVERIFY(SftpSessionSafetyTestAccess::running(*backendA));
        stack.setSessionContext(QStringLiteral("B"), &b);
        QCOMPARE(stack.currentPanel(), panelB);
        QCOMPARE(panelB->findChild<ElaLineEdit*>()->text(), QStringLiteral("/var/log/b"));
    }

    void renamingActiveSessionDoesNotReconnect()
    {
        SshTransport a(SshConfig{});
        SftpPanel panel;
        auto* backend = panel.findChild<SftpSession*>();
        panel.setSessionContext(QStringLiteral("A"), &a);
        SftpSessionSafetyTestAccess::connected(*backend, QStringLiteral("/home/a"));
        emit backend->directoryListed(QStringLiteral("/srv/a"), {});
        QSignalSpy errors(backend, &SftpSession::errorOccurred);
        panel.setSessionContext(QStringLiteral("renamed A"), &a);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(panel.windowTitle(), QStringLiteral("renamed A"));
        QCOMPARE(panel.findChild<ElaLineEdit*>()->text(), QStringLiteral("/srv/a"));
    }

    void closingSessionsReleasesOnlyTheirOwnPanels()
    {
        auto a = std::make_unique<SshTransport>(SshConfig{});
        auto b = std::make_unique<SshTransport>(SshConfig{});
        SshPanelStack<SftpPanel> stack;
        stack.setSessionContext(QStringLiteral("A"), a.get());
        QPointer<SftpPanel> panelA = stack.currentPanel();
        stack.setSessionContext(QStringLiteral("B"), b.get());
        QPointer<SftpPanel> panelB = stack.currentPanel();
        a.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(panelA.isNull());
        QCOMPARE(stack.currentPanel(), panelB.data());
        b.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(panelB.isNull());
        QCOMPARE(stack.count(), 1);
    }

    void monitorContinuesAcrossSessionSwitches()
    {
        SshTransport a(SshConfig{}), b(SshConfig{});
        SshTransportTestAccess::connected(a);
        SshTransportTestAccess::connected(b);
        SshPanelStack<SystemMonitorPanel> stack(nullptr, [](SystemMonitorPanel* panel) {
            panel->setBackgroundSamplingEnabled(true);
        });
        stack.setSessionContext(QStringLiteral("A"), &a);
        auto* panelA = stack.currentPanel();
        finishInitialBaseline(a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        const auto generation = SshTransportTestAccess::generation(a);
        stack.setSessionContext(QStringLiteral("B"), &b);
        finishInitialBaseline(b);
        QVERIFY(SshTransportTestAccess::monitoring(a));
        SshTransportTestAccess::finishSample(a, sample());
        periodicTick(*panelA);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        stack.setSessionContext(QStringLiteral("renamed A"), &a);
        QCOMPARE(stack.currentPanel(), panelA);
        QCOMPARE(SshTransportTestAccess::generation(a), generation);
        QCOMPARE(stack.windowTitle(), QStringLiteral("renamed A"));
        panelA->setPresentationActive(false);
        QVERIFY(!SshTransportTestAccess::monitoring(a));
        QVERIFY(SshTransportTestAccess::monitoring(b));
    }

    void periodicSamplingPreservesLocalResultsAndErrorBackoff()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        const quint64 first = SshTransportTestAccess::pendingSample(a);
        QTest::qWait(30);
        QCOMPARE(SshTransportTestAccess::pendingSample(a), first);
        SshTransportTestAccess::finishSample(a, sample());
        // Local calculation is already visible before the next request is posted.
        const auto bars = panel.findChildren<ElaProgressBar*>();
        QCOMPARE(bars.size(), 3);
        QCOMPARE(bars[1]->value(), 50);
        QCOMPARE(bars[2]->value(), 75);
        QCOMPARE(SshTransportTestAccess::pendingSample(a), quint64{0});
        periodicTick(panel);
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        SshTransportTestAccess::finishSample(a, {}, QStringLiteral("network error"));
        QTest::qWait(30);
        QCOMPARE(SshTransportTestAccess::pendingSample(a), quint64{0});
    }

    void filesystemQueriesShareThePersistentSampleChannel()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        panel.findChild<ElaIconButton*>()->click();
        QCoreApplication::processEvents();
        QVERIFY(!SshTransportTestAccess::commandPending(a));
        QVERIFY(!SshTransportTestAccess::includesFileSystems(a)); // 在途旧请求不改变。
        SshTransportTestAccess::finishSample(a, sample());
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        QVERIFY(SshTransportTestAccess::includesFileSystems(a));
        QVERIFY(!SshTransportTestAccess::commandPending(a));
        SshTransportTestAccess::finishSample(a, sample()
            + QByteArrayLiteral("@@filesystems\n/dev/sda 1000 500 500 50% /\n"));
        periodicTick(panel);
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        QVERIFY(!SshTransportTestAccess::includesFileSystems(a));
        panel.findChild<SystemInformationDialog*>()->close();
        QCoreApplication::processEvents();
        SshTransportTestAccess::finishSample(a, sample());
        periodicTick(panel);
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        QVERIFY(!SshTransportTestAccess::includesFileSystems(a));
    }

    void failedFilesystemQueryBacksOffWithoutLosingMetrics()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        panel.findChild<ElaIconButton*>()->click();
        QCoreApplication::processEvents();
        SshTransportTestAccess::finishSample(a, sample());
        QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        QVERIFY(SshTransportTestAccess::includesFileSystems(a));
        SshTransportTestAccess::finishSample(a, sample() + QByteArrayLiteral("@@filesystems\n"));
        QCOMPARE(panel.findChildren<ElaProgressBar*>()[1]->value(), 50);
        QTest::qWait(30);
        QCOMPARE(SshTransportTestAccess::pendingSample(a), quint64{0});
    }

    void deferredDetailsDoNotBlockBasicSampling()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        QElapsedTimer connectionClock;
        connectionClock.start();
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::pendingSample(a) != 0, 200);
        QVERIFY(!SshTransportTestAccess::commandPending(a));
        SshTransportTestAccess::finishSample(a, sample());
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::commandPending(a), 1500);
        QVERIFY(connectionClock.elapsed() >= 1100);
        // Keep the detail request in flight; a periodic basic sample still proceeds.
        periodicTick(panel);
        QVERIFY(SshTransportTestAccess::pendingSample(a) != 0);
        SshTransportTestAccess::finishSample(a, sample());
        QElapsedTimer batchClock;
        batchClock.start();
        SshTransportTestAccess::finishCommand(a, QByteArrayLiteral("@@done\n"));
        QTest::qWait(100);
        QVERIFY(!SshTransportTestAccess::commandPending(a));
        QTRY_VERIFY_WITH_TIMEOUT(SshTransportTestAccess::commandPending(a), 1500);
        QVERIFY(batchClock.elapsed() >= 900);
    }

    void repeatedCpuTicksKeepTheLastValidReading()
    {
        SshTransport a(SshConfig{});
        SshTransportTestAccess::connected(a);
        SystemMonitorPanel panel;
        panel.setBackgroundSamplingEnabled(true);
        panel.setSessionContext(QStringLiteral("A"), &a);
        finishInitialBaseline(a);
        for (int step : {0, 1, 2, 2}) {
            QTRY_VERIFY(SshTransportTestAccess::pendingSample(a) != 0);
            QByteArray current = sample();
            current.replace("cpu 100 0 50 200", QByteArray("cpu ")
                + QByteArray::number(100 + step * 10) + " 0 50 "
                + QByteArray::number(200 + step * 10));
            SshTransportTestAccess::finishSample(a, current);
            periodicTick(panel);
            if (step == 2) {
                const auto* cpu = panel.findChildren<ElaProgressBar*>()[0];
                QCOMPARE(cpu->value(), 50);
                QCOMPARE(cpu->format(), QStringLiteral("50%"));
            }
        }
    }
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    eApp->init();
    SftpPanelTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "SftpPanelTests.moc"
