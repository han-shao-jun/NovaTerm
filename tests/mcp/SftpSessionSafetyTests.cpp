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
