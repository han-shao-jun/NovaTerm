/** @file SftpSessionSafetyTests.cpp @brief SFTP 工作线程生命周期及取消发布回归。 */
#include "session/SftpSession.h"
#include "mcp/files/LocalFileAccess.h"
#include "mcp/contracts/Budget.h"
#include <libssh/sftp.h>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <algorithm>
#include <cstring>

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
    static void directoryResult(const std::shared_ptr<Control>& control, quint64 generation,
        quint64 request, SftpDirectoryResult result)
    {
        SftpSession::postDirectoryResult(control, generation, request, std::move(result));
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
        SftpFileUploadResult& result, bool consumesTemporary = false)
    {
        SftpSession::finalizeAuthorizedUpload(control, ready, guard, publish, cleanup, result, consumesTemporary);
    }

    static int publication(bool overwrite, const sftp_attributes_struct* target, int status,
        bool hardlink, bool rename, QString& error)
    { return int(SftpSession::authorizedUploadPublication(overwrite, target, status, 1000, hardlink, rename, error)); }

    static bool downloadSource(const sftp_attributes_struct* attributes, quint32 owner,
        const sftp_attributes_struct* original = nullptr)
    { return SftpSession::authorizedDownloadSource(attributes, owner, original); }

    static SftpFileUploadResult download(const SftpFileUploadRequest& request, quint64 total,
        const std::function<qint64(char*, qsizetype)>& read, const std::function<bool()>& close,
        const std::function<bool()>& guard, const std::function<bool()>& stopped,
        const std::function<void(quint64, quint64)>& progress, const std::function<void()>& ready)
    { return SftpSession::copyAuthorizedDownload(request, total, read, close, guard, stopped, progress, ready); }

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
    void acceptedDirectoryRequestSettlesOnNaturalWorkerExit() {
        SftpSession session;
        auto worker = SftpSessionSafetyTestAccess::control(session);
        worker->running = true; worker->connected = true;
        const auto request = session.requestDirectory("/tmp"); QVERIFY(request);
        QSignalSpy received(&session, &SftpSession::directoryRequestFinished);
        SftpSessionSafetyTestAccess::finishWorker(worker);
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toULongLong(), request);
        QVERIFY(!qvariant_cast<SftpDirectoryResult>(received.first().at(1)).error.isEmpty());
        QVERIFY(worker->directoryRequests.isEmpty());
        QCOMPARE(session.requestDirectory("/tmp"), quint64(0));
        SftpSessionSafetyTestAccess::finishWorker(worker);
        QCoreApplication::processEvents(); QCOMPARE(received.size(), 1);
    }
    void directoryRequestsAreBoundedCorrelatedAndIsolated()
    {
        SftpSession session;
        QCOMPARE(session.requestDirectory("/tmp"), quint64(0));
        auto worker = SftpSessionSafetyTestAccess::control(session);
        worker->running = true; worker->connected = true;
        QCOMPARE(session.requestDirectory("relative"), quint64(0));
        QCOMPARE(session.requestDirectory(QString(5000, 'x')), quint64(0));
        QCOMPARE(session.requestDirectory(QString("/tmp\nother")), quint64(0));
        const auto first = session.requestDirectory("/tmp");
        const auto second = session.requestDirectory(".");
        QVERIFY(first && second && first != second);
        QSignalSpy received(&session, &SftpSession::directoryRequestFinished);
        QSignalSpy legacy(&session, &SftpSession::directoryListed);
        SftpDirectoryResult result; result.path = "/tmp";
        SftpSessionSafetyTestAccess::directoryResult(worker, worker->generation, first, result);
        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toULongLong(), first);
        QCOMPARE(qvariant_cast<SftpDirectoryResult>(received.first().at(1)).path, QString("/tmp"));
        QCOMPARE(legacy.size(), 0);
        const auto generation = worker->generation.load();
        ++worker->generation;
        SftpSessionSafetyTestAccess::directoryResult(worker, generation, second, result);
        QCoreApplication::processEvents(); QCOMPARE(received.size(), 1);
        session.cancelDirectoryRequest(second);
        for (int i = 0; i < 20; ++i) static_cast<void>(session.requestDirectory("/tmp"));
        QVERIFY(worker->directoryRequests.size() <= 16);
        worker->running = false; worker->connected = false;
    }
    void authorizedOverwritePublicationPolicy_data()
    {
        QTest::addColumn<bool>("overwrite"); QTest::addColumn<QString>("target");
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("hardlink");
        QTest::addColumn<bool>("rename"); QTest::addColumn<int>("method");
        QTest::addColumn<QString>("error");
        QTest::newRow("default-no-probe") << false << QStringLiteral("absent") << -1 << true << false << 1 << QString{};
        QTest::newRow("default-needs-hardlink") << false << QStringLiteral("absent") << -1 << false << true << 0 << QStringLiteral("TRANSFER_NO_REPLACE_UNSUPPORTED");
        QTest::newRow("overwrite-existing") << true << QStringLiteral("regular") << SSH_FX_OK << false << true << 2 << QString{};
        QTest::newRow("overwrite-needs-rename") << true << QStringLiteral("regular") << SSH_FX_OK << true << false << 0 << QStringLiteral("TRANSFER_REPLACE_UNSUPPORTED");
        QTest::newRow("overwrite-absent-keeps-no-replace") << true << QStringLiteral("absent") << SSH_FX_NO_SUCH_FILE << true << true << 1 << QString{};
        QTest::newRow("absent-needs-hardlink") << true << QStringLiteral("absent") << SSH_FX_NO_SUCH_FILE << false << true << 0 << QStringLiteral("TRANSFER_NO_REPLACE_UNSUPPORTED");
        for (const auto status : {-1, int(SSH_FX_FAILURE), int(SSH_FX_PERMISSION_DENIED), int(SSH_FX_CONNECTION_LOST), int(SSH_FX_NO_SUCH_PATH)})
            QTest::newRow(qPrintable(QStringLiteral("lookup-error-%1").arg(status)))
                << true << QStringLiteral("absent") << status << true << true << 0 << QStringLiteral("TRANSFER_REMOTE_TARGET_UNAVAILABLE");
        for (const auto* kind : {"symlink", "directory", "device", "foreign", "writable", "missing-size", "missing-time", "missing-owner", "missing-mode"})
            QTest::newRow(kind) << true << QString::fromLatin1(kind) << SSH_FX_OK << true << true << 0 << QStringLiteral("TRANSFER_UNSAFE_REMOTE_TARGET");
        QTest::newRow("admin-target") << true << QStringLiteral("admin") << SSH_FX_OK << true << true << 2 << QString{};
    }

    void authorizedOverwritePublicationPolicy()
    {
        QFETCH(bool, overwrite); QFETCH(QString, target); QFETCH(int, status);
        QFETCH(bool, hardlink); QFETCH(bool, rename); QFETCH(int, method); QFETCH(QString, error);
        sftp_attributes_struct attributes{};
        attributes.type = SSH_FILEXFER_TYPE_REGULAR;
        attributes.flags = SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_SIZE
            | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
        attributes.uid = 1000; attributes.gid = 1000; attributes.permissions = 0100644;
        attributes.size = 7; attributes.mtime = 17;
        if (target == "symlink") attributes.type = SSH_FILEXFER_TYPE_SYMLINK;
        if (target == "directory") attributes.type = SSH_FILEXFER_TYPE_DIRECTORY;
        if (target == "device") attributes.type = SSH_FILEXFER_TYPE_SPECIAL;
        if (target == "foreign") attributes.uid = 1001;
        if (target == "admin") attributes.uid = 0;
        if (target == "writable") attributes.permissions |= 0020;
        if (target == "missing-size") attributes.flags &= ~SSH_FILEXFER_ATTR_SIZE;
        if (target == "missing-time") attributes.flags &= ~SSH_FILEXFER_ATTR_ACMODTIME;
        if (target == "missing-owner") attributes.flags &= ~SSH_FILEXFER_ATTR_UIDGID;
        if (target == "missing-mode") attributes.flags &= ~SSH_FILEXFER_ATTR_PERMISSIONS;
        QString actualError;
        QCOMPARE(SftpSessionSafetyTestAccess::publication(overwrite, target == "absent" ? nullptr : &attributes,
            status, hardlink, rename, actualError), method);
        QCOMPARE(actualError, error);
    }

    void authorizedRenameConsumesTemporaryOnlyWhenAcknowledged_data()
    {
        QTest::addColumn<int>("outcome"); QTest::addColumn<QString>("publication");
        QTest::newRow("acknowledged") << 0 << QStringLiteral("published");
        QTest::newRow("refused") << 1 << QStringLiteral("not_started");
        QTest::newRow("lost-ack") << 2 << QStringLiteral("unknown");
    }

    void authorizedRenameConsumesTemporaryOnlyWhenAcknowledged()
    {
        QFETCH(int, outcome); QFETCH(QString, publication);
        auto control = std::make_shared<SftpFileUploadControl>();
        SftpFileUploadResult result; int cleanups = 0;
        SftpSessionSafetyTestAccess::finalizeFile(control, [&] { control->approve(true); },
            [] { return true; }, [&] { return outcome; },
            [&] { ++cleanups; return QStringLiteral("retained"); }, result, true);
        QCOMPARE(result.published, outcome == 0); QCOMPARE(result.publication, publication);
        QCOMPARE(result.temporary, outcome == 0 ? QStringLiteral("removed") : QStringLiteral("retained"));
        QCOMPARE(cleanups, outcome == 0 ? 0 : 1);
    }

    void authorizedDownloadQueueSharesWorkerAndCancellation()
    {
        SftpSession session;
        auto worker = SftpSessionSafetyTestAccess::control(session);
        worker->running = true; worker->connected = true;
        SftpFileUploadRequest request{301, QStringLiteral("C:/root"), QStringLiteral("local.bin"),
            QStringLiteral("/home/test"), QStringLiteral("remote.bin"), std::make_shared<SftpFileUploadControl>()};
        QCOMPARE(request.direction, SftpFileTransferDirection::Upload);
        QVERIFY(!request.overwrite);
        request.overwrite = true;
        request.direction = SftpFileTransferDirection::Download;
        QVERIFY(session.uploadAuthorizedFile(request));
        QCOMPARE(worker->commands.head().fileUpload.direction, SftpFileTransferDirection::Download);
        QVERIFY(worker->commands.head().fileUpload.overwrite);
        QVERIFY(session.hasPendingOperations());
        auto upload = request; upload.direction = SftpFileTransferDirection::Upload;
        QVERIFY(!session.uploadAuthorizedFile(upload));
        QSignalSpy finished(&session, &SftpSession::authorizedFileFinished);
        SftpSessionSafetyTestAccess::finishWorker(worker);
        QTRY_COMPARE(finished.size(), 1);
        const auto result = qvariant_cast<SftpFileUploadResult>(finished.first().first());
        QCOMPARE(result.requestId, quint64(301)); QVERIFY(!result.published);
        QCOMPARE(request.control->state.load(), SftpFileUploadControl::State::Cancelled);
        QVERIFY(!session.hasPendingOperations());
    }

    void authorizedTransferRejectsInvalidDirection()
    {
        SftpSession session;
        auto worker = SftpSessionSafetyTestAccess::control(session);
        worker->running = true; worker->connected = true;
        SftpFileUploadRequest request{302, QStringLiteral("C:/root"), QStringLiteral("local.bin"),
            QStringLiteral("/home/test"), QStringLiteral("remote.bin"), std::make_shared<SftpFileUploadControl>()};
        request.direction = static_cast<SftpFileTransferDirection>(99);
        QVERIFY(!session.uploadAuthorizedFile(request));
        QVERIFY(!session.hasPendingOperations());
    }

    void authorizedDownloadSourceMetadata_data()
    {
        QTest::addColumn<QString>("change"); QTest::addColumn<bool>("allowed");
        for (const auto* change : {"unchanged", "atime", "admin"}) QTest::newRow(change) << QString::fromLatin1(change) << true;
        for (const auto* change : {"null", "symlink", "directory", "device", "foreign", "writable",
            "missing-size", "missing-owner", "missing-permissions", "missing-time", "changed-owner", "changed-group",
            "changed-mode", "changed-size", "changed-mtime"})
            QTest::newRow(change) << QString::fromLatin1(change) << false;
    }

    void authorizedDownloadSourceMetadata()
    {
        QFETCH(QString, change); QFETCH(bool, allowed);
        sftp_attributes_struct original{};
        original.type = SSH_FILEXFER_TYPE_REGULAR;
        original.flags = SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_SIZE
            | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
        original.uid = 1000; original.gid = 1000; original.permissions = 0100644;
        original.size = 7; original.mtime = 17; original.atime = 11;
        if (change == "admin") original.uid = 0;
        auto current = original;
        if (change == "symlink") current.type = SSH_FILEXFER_TYPE_SYMLINK;
        if (change == "directory") current.type = SSH_FILEXFER_TYPE_DIRECTORY;
        if (change == "device") current.type = SSH_FILEXFER_TYPE_SPECIAL;
        if (change == "foreign") current.uid = 1001;
        if (change == "writable") current.permissions |= 0020;
        if (change == "missing-size") current.flags &= ~SSH_FILEXFER_ATTR_SIZE;
        if (change == "missing-owner") current.flags &= ~SSH_FILEXFER_ATTR_UIDGID;
        if (change == "missing-permissions") current.flags &= ~SSH_FILEXFER_ATTR_PERMISSIONS;
        if (change == "missing-time") current.flags &= ~SSH_FILEXFER_ATTR_ACMODTIME;
        if (change == "changed-owner") current.uid = 0;
        if (change == "changed-group") ++current.gid;
        if (change == "changed-mode") current.permissions = 0100600;
        if (change == "changed-size") ++current.size;
        if (change == "changed-mtime") ++current.mtime;
        if (change == "atime") ++current.atime;
        QCOMPARE(SftpSessionSafetyTestAccess::downloadSource(change == "null" ? nullptr : &current,
            1000, &original), allowed);
    }

    void authorizedDownloadCopiesIntoSafeWriter_data()
    {
        QTest::addColumn<QString>("mode"); QTest::addColumn<QString>("expectedError");
        for (const auto* mode : {"complete", "partial", "empty"})
            QTest::newRow(mode) << QString::fromLatin1(mode) << QString{};
        QTest::newRow("overwrite-existing") << QStringLiteral("overwrite-existing") << QString{};
        QTest::newRow("overwrite-revoke") << QStringLiteral("overwrite-revoke") << QStringLiteral("TRANSFER_CANCELLED");
        QTest::newRow("overwrite-race") << QStringLiteral("overwrite-race") << QStringLiteral("TRANSFER_LOCAL_TARGET_EXISTS");
        QTest::newRow("early-eof") << QStringLiteral("early-eof") << QStringLiteral("TRANSFER_REMOTE_SIZE_CHANGED");
        QTest::newRow("growth") << QStringLiteral("growth") << QStringLiteral("TRANSFER_REMOTE_SIZE_CHANGED");
        QTest::newRow("read-error") << QStringLiteral("read-error") << QStringLiteral("TRANSFER_REMOTE_READ_FAILED");
        QTest::newRow("over-return") << QStringLiteral("over-return") << QStringLiteral("TRANSFER_REMOTE_READ_FAILED");
        QTest::newRow("cancel-read") << QStringLiteral("cancel-read") << QStringLiteral("TRANSFER_CANCELLED");
        QTest::newRow("guard-read") << QStringLiteral("guard-read") << QStringLiteral("TRANSFER_PATH_CHANGED");
        QTest::newRow("close-error") << QStringLiteral("close-error") << QStringLiteral("TRANSFER_REMOTE_CLOSE_FAILED");
        QTest::newRow("target-exists") << QStringLiteral("target-exists") << QStringLiteral("TRANSFER_LOCAL_TARGET_EXISTS");
        QTest::newRow("target-race") << QStringLiteral("target-race") << QStringLiteral("TRANSFER_LOCAL_TARGET_EXISTS");
        QTest::newRow("revoke-ready") << QStringLiteral("revoke-ready") << QStringLiteral("TRANSFER_CANCELLED");
        QTest::newRow("guard-ready") << QStringLiteral("guard-ready") << QStringLiteral("TRANSFER_PATH_CHANGED");
        QTest::newRow("size-limit") << QStringLiteral("size-limit") << QStringLiteral("TRANSFER_SIZE_LIMIT");
        QTest::newRow("pre-cancel") << QStringLiteral("pre-cancel") << QStringLiteral("TRANSFER_CANCELLED");
        QTest::newRow("initial-guard") << QStringLiteral("initial-guard") << QStringLiteral("TRANSFER_PATH_CHANGED");
    }

    void authorizedDownloadCopiesIntoSafeWriter()
    {
#ifndef Q_OS_WIN
        QSKIP("The existing LocalFileWriter native backend is Windows-only.");
#else
        QFETCH(QString, mode); QFETCH(QString, expectedError);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        SftpFileUploadRequest request{303, directory.path(), QStringLiteral("result.bin"),
            QStringLiteral("/home/test"), QStringLiteral("remote.bin"), std::make_shared<SftpFileUploadControl>(),
            SftpFileTransferDirection::Download};
        const auto target = directory.filePath(request.localRelativePath);
        QByteArray payload = mode == "empty" ? QByteArray{} : QByteArray(70003, 'x');
        quint64 total = quint64(payload.size());
        if (mode == "early-eof") ++total;
        if (mode == "growth") --total;
        if (mode == "size-limit") total = NovaTerm::Mcp::Budget::MaxLocalTransferFileBytes + 1;
        bool fixtureOk = true;
        const auto createCompetitor = [&] {
            QFile other(target);
            fixtureOk = other.open(QIODevice::WriteOnly) && other.write("keep") == 4;
        };
        request.overwrite = mode.startsWith(QStringLiteral("overwrite-"));
        if (mode == "target-exists" || mode == "overwrite-existing" || mode == "overwrite-revoke") createCompetitor();
        if (mode == "pre-cancel") request.control->cancel();
        bool safe = mode != "initial-guard", closedBeforeReady = false;
        int reads = 0, closes = 0, publications = 0;
        qsizetype offset = 0, maxOffered = 0;
        quint64 reported = 0;
        const auto result = SftpSessionSafetyTestAccess::download(request, total,
            [&](char* bytes, qsizetype count) -> qint64 {
                ++reads; maxOffered = std::max(maxOffered, count);
                if (mode == "read-error") return -1;
                if (mode == "over-return") return count + 1;
                if (mode == "cancel-read") request.control->cancel();
                if (mode == "guard-read") safe = false;
                const auto amount = std::min({count, payload.size() - offset, qsizetype(mode == "partial" ? 2048 : 65536)});
                if (amount > 0) std::memcpy(bytes, payload.constData() + offset, size_t(amount));
                offset += amount; return amount;
            }, [&] { ++closes; return mode != "close-error"; },
            [&] { return safe; }, [] { return false; },
            [&](quint64 transferred, quint64) { reported = transferred; },
            [&] {
                ++publications; closedBeforeReady = closes == 1;
                if (mode == "target-race" || mode == "overwrite-race") createCompetitor();
                if (mode == "revoke-ready" || mode == "overwrite-revoke") request.control->cancel();
                if (mode == "guard-ready") safe = false;
                request.control->approve(true);
            });
        QVERIFY(fixtureOk); QCOMPARE(result.requestId, quint64(303));
        QCOMPARE(result.errorCode, expectedError); QCOMPARE(closes, 1);
        QVERIFY(maxOffered <= 65536);
        QCOMPARE(result.published, expectedError.isEmpty());
        if (expectedError.isEmpty()) {
            QCOMPARE(result.publication, QStringLiteral("published"));
            QCOMPARE(result.totalBytes, std::optional<quint64>(total));
            QCOMPARE(result.transferredBytes, total); QCOMPARE(reported, total);
            QCOMPARE(publications, 1); QVERIFY(closedBeforeReady);
            QVERIFY(!request.control->cancel());
            QFile file(target); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), payload);
        } else {
            QCOMPARE(result.publication, QStringLiteral("not_started"));
            if (mode == "target-exists" || mode == "target-race" || mode == "overwrite-revoke" || mode == "overwrite-race") {
                QFile file(target); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("keep"));
            } else QVERIFY(!QFile::exists(target));
            if (mode == "target-exists" || mode == "size-limit" || mode == "pre-cancel" || mode == "initial-guard") QCOMPARE(reads, 0);
            if (mode == "cancel-read" || mode == "guard-read") QCOMPARE(result.transferredBytes, quint64(0));
        }
        QCOMPARE(QDir(directory.path()).entryList({QStringLiteral(".novaterm-transfer-*")}, QDir::Files | QDir::Hidden).size(), 0);
#endif
    }

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
