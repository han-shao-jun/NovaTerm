/** @file SftpSessionSafetyTests.cpp @brief SFTP 工作线程生命周期及取消发布回归。 */
#include "session/SftpSession.h"

#include <QCoreApplication>
#include <QMutexLocker>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>

class SftpSessionSafetyTestAccess
{
public:
    using Control = SftpSession::WorkerControl;

    static std::shared_ptr<Control> control(SftpSession& session)
    {
        return session._control;
    }

    static void postError(const std::shared_ptr<Control>& control)
    {
        SftpSession::postError(control, control->generation, QStringLiteral("test"));
    }

    static QThread* pauseWorker(SftpSession& session, QSemaphore& entered,
                                QSemaphore& resume, std::atomic<bool>& finished,
                                std::atomic<bool>& cancelled)
    {
        auto control = session._control;
        control->running = true;
        control->connected = true;
        control->activeUploadRequestId = 7;
        control->queuedUploadBytes = 11;
        const quint64 generation = control->generation;
        session._thread = QThread::create(
            [control, generation, &entered, &resume, &finished, &cancelled] {
                entered.release();
                resume.acquire();
                cancelled = SftpSession::isUploadCancelled(control, 7);
                {
                    QMutexLocker lock(&control->queueMutex);
                    control->queuedUploadBytes -= 11;
                    control->activeUploadRequestId = 0;
                    control->cancelledUploadRequests.remove(7);
                }
                SftpSession::postError(control, generation, QStringLiteral("late"));
                SftpSession::finishWorker(control, generation);
                finished = true;
            });
        session._thread->start();
        return session._thread;
    }

    static void abandon(SftpSession& session)
    {
        session.stopWorker(0);
    }

    static void finishWorker(const std::shared_ptr<Control>& control)
    {
        SftpSession::finishWorker(control, control->generation);
    }

    static bool uploadOwners(const QVector<quint32>& owners)
    {
        return SftpSession::authorizedUploadOwners(owners);
    }

    static void finalizeFile(const std::shared_ptr<SftpFileUploadControl>& control,
        const std::function<void()>& ready, const std::function<bool()>& guard,
        const std::function<int()>& publish, const std::function<QString()>& cleanup,
        SftpFileUploadResult& result)
    {
        SftpSession::finalizeAuthorizedUpload(control, ready, guard, publish, cleanup, result);
    }

    static bool finalize(const std::function<bool()>& cancelled,
                         const std::function<bool()>& close,
                         const std::function<bool()>& chmod,
                         const std::function<bool()>& publish,
                         const std::function<void()>& discard, QString& error)
    {
        return SftpSession::finalizeScriptUpload(
            cancelled, close, chmod, publish, discard, error);
    }
};

class SftpSessionSafetyTests final : public QObject
{
    Q_OBJECT

private slots:
    void queuedNotificationsExpireWithReceiver()
    {
        int delivered = 0;
        auto session = std::make_unique<SftpSession>();
        const auto control = SftpSessionSafetyTestAccess::control(*session);
        connect(session.get(), &SftpSession::errorOccurred, this,
                [&] { ++delivered; });
        SftpSessionSafetyTestAccess::postError(control);
        session.reset();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(delivered, 0);
        SftpSessionSafetyTestAccess::postError(control);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(delivered, 0);
    }

    void notificationFromPreviousGenerationIsIgnored()
    {
        SftpSession session;
        QSignalSpy errors(&session, &SftpSession::errorOccurred);
        auto control = SftpSessionSafetyTestAccess::control(session);
        SftpSessionSafetyTestAccess::postError(control);
        ++control->generation;
        QCoreApplication::sendPostedEvents();
        QCOMPARE(errors.size(), 0);
        SftpSessionSafetyTestAccess::postError(control);
        QTRY_COMPARE(errors.size(), 1);
    }

    void abandonedWorkerResumesAfterReceiverDestruction()
    {
        QSemaphore entered;
        QSemaphore resume;
        std::atomic<bool> finished{false};
        std::atomic<bool> cancelled{false};
        auto session = std::make_unique<SftpSession>();
        auto control = SftpSessionSafetyTestAccess::control(*session);
        std::weak_ptr<SftpSessionSafetyTestAccess::Control> lifetime = control;
        QPointer<QThread> worker = SftpSessionSafetyTestAccess::pauseWorker(
            *session, entered, resume, finished, cancelled);
        const auto releaseWorker = qScopeGuard([&] {
            resume.release();
            if (worker)
                worker->wait(5000);
        });
        QVERIFY(entered.tryAcquire(1, 5000));
        SftpSessionSafetyTestAccess::abandon(*session);
        session.reset();
        QVERIFY(!control->receiver);
        QVERIFY(!control->running);
        // Active bytes remain owned by the abandoned worker until it resumes.
        QCOMPARE(control->queuedUploadBytes, qsizetype(11));
        resume.release();
        QTRY_VERIFY(finished.load());
        QVERIFY(cancelled.load());
        QCOMPARE(control->queuedUploadBytes, qsizetype(0));
        QVERIFY(!control->connected);
        control.reset();
        QTRY_VERIFY(worker.isNull());
        QTRY_VERIFY(lifetime.expired());
    }

    void cancellationAtPublicationBoundaries_data()
    {
        QTest::addColumn<QString>("cancelAt");
        QTest::addColumn<bool>("published");
        QTest::newRow("before-close") << QStringLiteral("before") << false;
        QTest::newRow("during-close") << QStringLiteral("close") << false;
        QTest::newRow("during-chmod") << QStringLiteral("chmod") << false;
        QTest::newRow("during-rename") << QStringLiteral("rename") << true;
        QTest::newRow("no-cancellation") << QString{} << true;
    }

    void cancellationAtPublicationBoundaries()
    {
        QFETCH(QString, cancelAt);
        QFETCH(bool, published);
        bool cancelled = cancelAt == QStringLiteral("before");
        QStringList operations;
        QString error;
        const auto operation = [&](const QString& name) {
            operations.append(name);
            if (cancelAt == name)
                cancelled = true;
            return true;
        };
        const bool success = SftpSessionSafetyTestAccess::finalize(
            [&] { return cancelled; },
            [&] { return operation(QStringLiteral("close")); },
            [&] { return operation(QStringLiteral("chmod")); },
            [&] { return operation(QStringLiteral("rename")); },
            [&] { operations.append(QStringLiteral("discard")); }, error);
        QCOMPARE(success, published);
        QCOMPARE(operations.contains(QStringLiteral("rename")), published);
        QCOMPARE(operations.contains(QStringLiteral("discard")), !published);
        QCOMPARE(operations.constFirst(), QStringLiteral("close"));
        QCOMPARE(error, published ? QString{} : QStringLiteral("SCRIPT_UPLOAD_CANCELLED"));
    }

    void workerExitSettlesAcceptedFileUpload_data()
    {
        QTest::addColumn<bool>("stopBeforeDelivery");
        QTest::newRow("natural-exit") << false;
        QTest::newRow("natural-exit-then-explicit-stop") << true;
    }

    void workerExitSettlesAcceptedFileUpload()
    {
        QFETCH(bool, stopBeforeDelivery);
        SftpSession session;
        auto worker = SftpSessionSafetyTestAccess::control(session);
        worker->running = true; worker->connected = true;
        const SftpFileUploadRequest request{91, QStringLiteral("C:/root"),
            QStringLiteral("data.bin"), QStringLiteral("/home/test"),
            QStringLiteral("data.bin"), std::make_shared<SftpFileUploadControl>()};
        QSignalSpy finished(&session, &SftpSession::authorizedFileFinished);
        QVERIFY(session.uploadAuthorizedFile(request));
        // The previous command has finished, but the next iteration detects a
        // disconnected SSH channel before dequeuing this accepted command.
        SftpSessionSafetyTestAccess::finishWorker(worker);
        if (stopBeforeDelivery) session.disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 200);
        const auto result = qvariant_cast<SftpFileUploadResult>(finished.first().first());
        QCOMPARE(result.requestId, quint64(91));
        QVERIFY(!result.published); QCOMPARE(result.publication, QStringLiteral("not_started"));
        QCOMPARE(result.temporary, QStringLiteral("none"));
        QVERIFY(!result.errorCode.isEmpty());
        QCOMPARE(request.control->state.load(), SftpFileUploadControl::State::Cancelled);
        QVERIFY(!session.hasPendingOperations());
        QVERIFY(!session.uploadAuthorizedFile(request));
        session.disconnectFromHost();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(finished.size(), 1);
    }

    void authorizedUploadAncestorOwners_data()
    {
        QTest::addColumn<QVector<quint32>>("owners");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("admin-and-scope-owner") << QVector<quint32>{0, 0, 1000, 1000} << true;
        QTest::newRow("admin-root") << QVector<quint32>{0} << true;
        QTest::newRow("scope-only") << QVector<quint32>{1000} << true;
        QTest::newRow("foreign-ancestor") << QVector<quint32>{0, 1001, 1000} << false;
        QTest::newRow("ancestor-owner-changed") << QVector<quint32>{1001, 1000, 1000} << false;
        QTest::newRow("scope-owner-changed") << QVector<quint32>{0, 1000, 1001} << false;
        QTest::newRow("foreign-before-admin-root") << QVector<quint32>{0, 1001, 0} << false;
        QTest::newRow("missing-owner") << QVector<quint32>{} << false;
    }

    void authorizedUploadAncestorOwners()
    {
        QFETCH(QVector<quint32>, owners);
        QFETCH(bool, allowed);
        QCOMPARE(SftpSessionSafetyTestAccess::uploadOwners(owners), allowed);
    }

    void fileUploadAdmissionRequiresConnectedIdleWorker()
    {
        SftpSession session;
        auto worker = SftpSessionSafetyTestAccess::control(session);
        SftpFileUploadRequest request{9, QStringLiteral("C:/root"),
            QStringLiteral("data.bin"), QStringLiteral("/home/test"),
            QStringLiteral("data.bin"), std::make_shared<SftpFileUploadControl>()};
        QVERIFY(!session.uploadAuthorizedFile(request));
        worker->running = true;
        worker->connected = true;
        worker->commandActive = true;
        QVERIFY(!session.uploadAuthorizedFile(request));
        worker->commandActive = false;
        QVERIFY(session.uploadAuthorizedFile(request));
        QVERIFY(session.hasPendingOperations());
        QVERIFY(!session.uploadAuthorizedFile(request));
    }

    void authorizedPublicationPreservesActualOutcome_data()
    {
        QTest::addColumn<int>("outcome");
        QTest::addColumn<QString>("publication");
        QTest::newRow("success") << 0 << QStringLiteral("published");
        QTest::newRow("server-refusal") << 1 << QStringLiteral("not_started");
        QTest::newRow("lost-ack") << 2 << QStringLiteral("unknown");
    }

    void authorizedPublicationPreservesActualOutcome()
    {
        QFETCH(int, outcome);
        QFETCH(QString, publication);
        auto control = std::make_shared<SftpFileUploadControl>();
        SftpFileUploadResult result;
        int links = 0;
        bool cancelRejected = false;
        SftpSessionSafetyTestAccess::finalizeFile(control,
            [&] { control->approve(true); }, [] { return true; },
            [&] { ++links; cancelRejected = !control->cancel(); return outcome; },
            [] { return QStringLiteral("retained"); }, result);
        QCOMPARE(links, 1);
        QVERIFY(cancelRejected);
        QCOMPARE(result.publication, publication);
        QCOMPARE(result.published, outcome == 0);
        QCOMPARE(result.temporary, QStringLiteral("retained"));
    }

    void authorizationRevocationWinsBeforePublication()
    {
        auto control = std::make_shared<SftpFileUploadControl>();
        SftpFileUploadResult result;
        int links = 0;
        SftpSessionSafetyTestAccess::finalizeFile(control,
            [&] { control->cancel(); control->approve(true); },
            [] { return true; }, [&] { ++links; return 0; },
            [] { return QStringLiteral("removed"); }, result);
        QCOMPARE(links, 0);
        QCOMPARE(result.publication, QStringLiteral("not_started"));
        QCOMPARE(result.temporary, QStringLiteral("removed"));
        QCOMPARE(result.errorCode, QStringLiteral("TRANSFER_CANCELLED"));
    }

    void failedRenameRetainsUnknownPublicationOutcome()
    {
        bool discarded = false;
        QString error;
        QVERIFY(!SftpSessionSafetyTestAccess::finalize(
            [] { return false; }, [] { return true; }, [] { return true; },
            [] { return false; }, [&] { discarded = true; }, error));
        QVERIFY(discarded);
        QCOMPARE(error, QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN"));
    }
};

QTEST_GUILESS_MAIN(SftpSessionSafetyTests)
#include "SftpSessionSafetyTests.moc"
