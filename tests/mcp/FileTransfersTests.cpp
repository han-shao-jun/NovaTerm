#include "mcp/FileTransfers.h"
#include "core/terminal/TerminalCore.h"
#include "transport/SshTransport.h"
#include <QDir>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace NovaTerm::Mcp;
class SshTransportTestAccess {
public:
    static void connected(SshTransport& transport)
    {
        transport._connected = true;
        transport._serverHostKeyFingerprint = QString(64, QLatin1Char('a'));
    }
};
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
class LocalTransport final : public ITransport {
public:
    bool connectToHost() override { online = true; emit connected(); return true; }
    void disconnect() override { if (online) { online = false; emit disconnected(); } }
    void write(const QByteArray&) override {}
    void resizeTerminal(int, int) override {}
    bool isConnected() const override { return online; }
    QString errorString() const override { return {}; }
    bool online{false};
};
struct Fixture {
    QTemporaryDir root{QDir::currentPath() + "/novaterm-transfer-fixture-XXXXXX"};
    TerminalCore core{80, 24};
    LocalTransport local;
    std::unique_ptr<SshTransport> ssh;
    TerminalSession session{&core};
    AccessStore access{root.filePath("state"), std::make_unique<MemoryCredentialStore>()};
    SessionDirectory directory;
    FileResourceAdapter resources{access, directory, nullptr};
    QList<SftpFileUploadRequest> requests;
    QList<FileTransfers::Delivery> deliveries;
    qint64 now{0};
    FileTransfers transfers{access, directory, resources, "fixture-instance", nullptr,
        [this](TerminalSession&, const SftpFileUploadRequest& request, FileTransfers::Delivery delivery) {
            requests.append(request); deliveries.append(std::move(delivery)); return true;
        }, [this] { return now; }};
    QString client;
    bool ready{false};
    Fixture()
    {
        session.attach(&local, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        static_cast<void>(session.start());
        SshConfig config; config.host = "fixture.invalid"; config.username = "fixture";
        ssh = std::make_unique<SshTransport>(config);
        SshTransportTestAccess::connected(*ssh);
        session.attach(ssh.get(), TerminalSession::Ownership::Borrowed, TransportKind::Ssh);
        directory.add(&session);
        client = access.ensureAccess();
        ready = root.isValid() && !client.isEmpty() && access.setEnabled(true);
    }
    SessionDirectory::Entry entry() { return directory.entries().first(); }
    quint64 identity(const QString& owner = {}) const
    { return access.identityVersion(owner.isEmpty() ? client : owner); }
    StartFileUploadRequest grant()
    {
        const auto current = entry();
        StartFileUploadRequest request;
        request.reference = {NovaTerm::Mcp::SessionId{current.id}, Epoch{current.epoch}};
        request.localPath = root.filePath("build/firmware.bin");
        request.remotePath = "/tmp/nvt-artifacts/firmware-build.bin"; request.operationId = uuid();
        return request;
    }
    Result<QJsonObject> call(const FileTransferRequest& request, const QString& owner = {}, const QString& connection = "connection")
    {
        const auto who = owner.isEmpty() ? client : owner;
        return transfers.call(who, identity(who), connection, request);
    }
    Result<QJsonObject> query(const QString& transfer, const QString& owner = {})
    { return call(QueryFileTransferRequest{transfer}, owner); }
    StartFileDownloadRequest downloadGrant(bool bothDirections = false)
    {
        const auto current = entry();
        Q_UNUSED(bothDirections)
        StartFileDownloadRequest request;
        request.reference = {NovaTerm::Mcp::SessionId{current.id}, Epoch{current.epoch}};
        request.localPath = root.filePath("build/downloads/result.bin");
        request.remotePath = "/tmp/nvt-artifacts/capture.bin"; request.operationId = uuid();
        return request;
    }
    void finish(bool published)
    {
        SftpFileUploadResult result;
        result.requestId = requests.first().requestId;
        result.totalBytes = 7; result.transferredBytes = published ? 7 : 0;
        result.published = published;
        result.publication = published ? "published" : "not_started";
        result.temporary = "removed";
        result.errorCode = published ? "" : "CANCELLED";
        deliveries.first().finished(result);
    }
};
QJsonObject value(const Result<QJsonObject>& result)
{ return std::holds_alternative<QJsonObject>(result) ? std::get<QJsonObject>(result) : QJsonObject{}; }
QString transferId(const Result<QJsonObject>& result) { return value(result).value("transferId").toString(); }
StartFileUploadRequest uploadRequest(const StartFileDownloadRequest& request)
{
    return {request.reference, request.localPath, request.remotePath, request.operationId, request.overwrite, {}, {}};
}
}
class FileTransfersTests : public QObject {
    Q_OBJECT
private slots:
    void indexDeadlinePreservesTimeoutThroughWorkerCancellation() {
        Fixture f; QVERIFY(f.ready); const auto request = f.grant();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        f.now = 120000;
        const auto waiting = value(f.query(transferId(accepted)));
        QCOMPARE(waiting.value("state").toString(), QString("cancel_requested"));
        f.finish(false);
        const auto failed = value(f.query(transferId(accepted)));
        QCOMPARE(failed.value("state").toString(), QString("failed"));
        QCOMPARE(failed.value("errorCode").toString(), QString("TRANSFER_TIMEOUT"));
        QCOMPARE(value(f.call(request)), failed); QCOMPARE(f.requests.size(), 1);
    }
    void internalAbortPreservesFailureInsteadOfReportingUserCancellation_data()
    {
        QTest::addColumn<QString>("errorCode");
        QTest::addColumn<QString>("publication");
        QTest::addColumn<QString>("expectedState");
        QTest::newRow("timeout") << QString("TRANSFER_TIMEOUT") << QString("not_started") << QString("failed");
        QTest::newRow("path-changed") << QString("TRANSFER_PATH_CHANGED") << QString("not_started") << QString("failed");
        QTest::newRow("cancelled") << QString("TRANSFER_CANCELLED") << QString("not_started") << QString("cancelled");
        QTest::newRow("unknown") << QString("TRANSFER_TIMEOUT") << QString("unknown") << QString("unknown");
        QTest::newRow("published") << QString("TRANSFER_TIMEOUT") << QString("published") << QString("completed");
    }
    void internalAbortPreservesFailureInsteadOfReportingUserCancellation()
    {
        QFETCH(QString, errorCode); QFETCH(QString, publication); QFETCH(QString, expectedState);
        Fixture f; QVERIFY(f.ready); const auto accepted = f.call(f.grant()); QVERIFY(!isError(accepted));
        f.requests.first().control->cancel();
        SftpFileUploadResult result;
        result.requestId = f.requests.first().requestId; result.errorCode = errorCode;
        result.publication = publication; result.published = publication == "published";
        result.temporary = "removed"; result.totalBytes = 7;
        f.deliveries.first().finished(result);
        const auto actual = value(f.query(transferId(accepted)));
        QCOMPARE(actual.value("state").toString(), expectedState);
        QCOMPARE(actual.value("errorCode").toString(), errorCode);
        QCOMPARE(actual.value("publication").toString(), publication);
    }
    void directRootPathsAndUnicodeReachOriginalWorker()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.grant();
#ifdef Q_OS_WIN
        request.localPath = "C:/build artifact-测试.bin";
#else
        request.localPath = "/build artifact-测试.bin";
#endif
        request.remotePath = "/target artifact-测试.bin";
        QVERIFY(!isError(f.call(request)));
        QCOMPARE(f.requests.size(), 1);
#ifdef Q_OS_WIN
        QCOMPARE(f.requests.first().localRoot, QString("C:/"));
#else
        QCOMPARE(f.requests.first().localRoot, QString("/"));
#endif
        QCOMPARE(f.requests.first().localRelativePath, QString("build artifact-测试.bin"));
        QCOMPARE(f.requests.first().remoteRoot, QString("/"));
        QCOMPARE(f.requests.first().remoteName, QString("target artifact-测试.bin"));
    }
    void tokenRotationCancelsOriginalIdentityBeforePublication()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.grant();
        const auto oldIdentity = f.identity();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        f.requests.first().control->state.store(SftpFileUploadControl::State::AwaitingPublication);
        QVERIFY(f.access.rotateToken());
        QVERIFY(f.identity() != oldIdentity);
        f.deliveries.first().publicationReady();
        QCOMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Cancelled);
        QVERIFY(isError(f.transfers.call(f.client, oldIdentity, "connection", request)));
        f.finish(false);
        QVERIFY(isError(f.query(transferId(accepted))));
        QCOMPARE(f.requests.size(), 1);
    }
    void overwriteIntentIsPartOfRetryIdentity_data()
    {
        QTest::addColumn<bool>("download");
        QTest::newRow("upload") << false;
        QTest::newRow("download") << true;
    }
    void overwriteIntentIsPartOfRetryIdentity()
    {
        QFETCH(bool, download);
        Fixture f; QVERIFY(f.ready); auto request = f.downloadGrant(true);
        const auto call = [&](bool overwrite) {
            request.overwrite = overwrite;
            if (download) return f.call(request);
            auto upload = uploadRequest(request); upload.overwrite = overwrite;
            return f.call(upload);
        };
        const auto accepted = call(false); QVERIFY(!isError(accepted));
        QCOMPARE(transferId(call(false)), transferId(accepted)); QCOMPARE(f.requests.size(), 1);
        const auto conflict = call(true); QVERIFY(isError(conflict));
        QCOMPARE(std::get<Error>(conflict).code, ErrorCode::TargetOperationConflict);
        QCOMPARE(f.requests.size(), 1);
    }
    void explicitOverwriteReachesOriginalWorker_data() { overwriteIntentIsPartOfRetryIdentity_data(); }
    void explicitOverwriteReachesOriginalWorker()
    {
        QFETCH(bool, download);
        Fixture f; QVERIFY(f.ready); auto request = f.downloadGrant(true); request.overwrite = true;
        auto upload = uploadRequest(request); upload.overwrite = true;
        const auto result = download ? f.call(request) : f.call(upload); QVERIFY(!isError(result));
        QCOMPARE(f.requests.size(), 1); QVERIFY(f.requests.first().overwrite);
        QCOMPARE(f.requests.first().direction, download ? SftpFileTransferDirection::Download : SftpFileTransferDirection::Upload);
    }
    void downloadNeedsNoDirectoryGrantsAndPreservesLocalPublication()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.downloadGrant();
        QVERIFY(f.resources.resources().local(f.client, f.identity()).isEmpty());
        QVERIFY(f.resources.resources().remoteSessions().isEmpty());
        QVERIFY(!f.access.canRead(f.client, f.entry()));
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted); QVERIFY(!transfer.isEmpty());
        QCOMPARE(f.requests.size(), 1); QCOMPARE(f.requests.first().direction, SftpFileTransferDirection::Download);
        QCOMPARE(f.requests.first().localRoot, f.root.filePath("build/downloads"));
        QCOMPARE(f.requests.first().localRelativePath, QString("result.bin"));
        QCOMPARE(f.requests.first().remoteRoot, QString("/tmp/nvt-artifacts"));
        QCOMPARE(f.requests.first().remoteName, QString("capture.bin"));
        QVERIFY(value(accepted).value("totalBytes").isNull());
        f.deliveries.first().progress(3, 7);
        QCOMPARE(value(f.query(transfer)).value("state").toString(), QString("downloading"));
        f.requests.first().control->state.store(SftpFileUploadControl::State::AwaitingPublication);
        f.deliveries.first().publicationReady();
        QCOMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Approved);
        f.requests.first().control->state.store(SftpFileUploadControl::State::Publishing);
        f.finish(true);
        const auto completed = value(f.query(transfer));
        QCOMPARE(completed.value("state").toString(), QString("completed"));
        QVERIFY(completed.value("published").toBool());
        QCOMPARE(completed.value("publication").toString(), QString("published"));
        QCOMPARE(completed.value("temporary").toString(), QString("removed"));
        QCOMPARE(completed.value("transferredBytes").toInt(), 7);
        QVERIFY(!completed.value("contentVerified").toBool(true));
        QVERIFY(!f.transfers.busy(f.entry().id));
        QCOMPARE(value(f.call(request)), completed);
        QCOMPARE(value(f.call(CancelFileTransferRequest{transfer})), completed);
    }
    void obsoleteDirectoryGrantsDoNotGateDownloads_data()
    {
        QTest::addColumn<bool>("localWrite"); QTest::addColumn<bool>("remoteRead");
        QTest::newRow("write-read") << true << true;
        QTest::newRow("read-read") << false << true;
        QTest::newRow("write-write") << true << false;
        QTest::newRow("upload-permissions-only") << false << false;
    }
    void obsoleteDirectoryGrantsDoNotGateDownloads()
    {
        QFETCH(bool, localWrite); QFETCH(bool, remoteRead);
        Fixture f; QVERIFY(f.ready); auto request = f.downloadGrant();
        const auto localRoot = f.resources.resources().shareLocal(f.client, f.identity(), f.root.filePath("build"), "Local", {!localWrite, localWrite});
        const auto binding = f.access.grantSnapshot(ClientId{f.client}, f.entry()).binding;
        const auto remote = f.resources.shareRemote(f.client, f.identity(), binding, "/tmp/nvt-artifacts", "Remote", {remoteRead, !remoteRead});
        QVERIFY(localRoot); QVERIFY(remote);
        const auto result = f.call(request);
        QVERIFY(!isError(result));
        QCOMPARE(f.requests.size(), 1);
    }
    void downloadRetriesIncludeDirectionAndEveryArgument()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.downloadGrant(true);
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted);
        QCOMPARE(transferId(f.call(request, {}, "other-connection")), transfer);
        for (int field = 0; field < 4; ++field) {
            auto changed = request;
            if (field == 0) changed.reference.session = NovaTerm::Mcp::SessionId{uuid()};
            if (field == 1) changed.reference.epoch = Epoch{uuid()};
            if (field == 2) changed.localPath += ".other";
            if (field == 3) changed.remotePath += ".other";
            const auto denied = f.call(changed); QVERIFY(isError(denied));
            QCOMPARE(std::get<Error>(denied).code, ErrorCode::TargetOperationConflict);
        }
        const auto upload = f.call(uploadRequest(request)); QVERIFY(isError(upload));
        QCOMPARE(std::get<Error>(upload).code, ErrorCode::TargetOperationConflict);
        f.finish(true);
        const auto uploadAfter = f.call(uploadRequest(request)); QVERIFY(isError(uploadAfter));
        QCOMPARE(std::get<Error>(uploadAfter).code, ErrorCode::TargetOperationConflict);
        QCOMPARE(f.requests.size(), 1);
    }
    void downloadRevocationBeforePublication_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("access-disabled") << 0;
        QTest::newRow("ssh-disconnected") << 1;
        QTest::newRow("connection-replaced") << 2;
        QTest::newRow("rpc-disconnected") << 3;
    }
    void downloadRevocationBeforePublication()
    {
        QFETCH(int, change); Fixture f; QVERIFY(f.ready); const auto request = f.downloadGrant();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted);
        f.deliveries.first().progress(7, 7);
        f.requests.first().control->state.store(SftpFileUploadControl::State::AwaitingPublication);
        if (change == 0) QVERIFY(f.access.setEnabled(false));
        if (change == 1) f.ssh->disconnect();
        if (change == 2) f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        if (change == 3) f.transfers.disconnect("connection");
        f.deliveries.first().publicationReady();
        QCOMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Cancelled);
        f.finish(false);
        if (change == 0) {
            QVERIFY(f.access.setEnabled(true));
            // Re-enabling creates a new identity: old transfer facts are not
            // disclosed to it. Publication cancellation was checked above.
            const auto denied = f.query(transfer);
            QVERIFY(isError(denied));
            QCOMPARE(std::get<Error>(denied).code, ErrorCode::PermissionDenied);
            return;
        }
        const auto cancelled = value(f.query(transfer));
        QCOMPARE(cancelled.value("state").toString(), change == 3 ? QString("cancelled") : QString("failed"));
        QCOMPARE(cancelled.value("errorCode").toString(), change == 3 ? QString("CANCELLED") : QString("PERMISSION_DENIED"));
        QVERIFY(!cancelled.value("published").toBool());
        QCOMPARE(cancelled.value("transferredBytes").toInt(), 7);
    }
    void uploadAndDownloadShareBusyBudgetAndRequestIds()
    {
        Fixture f; QVERIFY(f.ready); auto download = f.downloadGrant(true);
        const auto first = f.call(download); QVERIFY(!isError(first));
        auto upload = uploadRequest(download); upload.operationId = uuid();
        const auto busyUpload = f.call(upload); QVERIFY(isError(busyUpload));
        QCOMPARE(std::get<Error>(busyUpload).code, ErrorCode::FileTransferBusy);
        f.finish(true);
        const auto second = f.call(upload); QVERIFY(!isError(second));
        QCOMPARE(f.requests.size(), 2);
        QVERIFY(f.requests.at(1).requestId > f.requests.first().requestId);
        QCOMPARE(f.requests.at(1).direction, SftpFileTransferDirection::Upload);
        download.operationId = uuid();
        const auto busyDownload = f.call(download); QVERIFY(isError(busyDownload));
        QCOMPARE(std::get<Error>(busyDownload).code, ErrorCode::FileTransferBusy);
        QVERIFY(!isError(f.call(CancelFileTransferRequest{transferId(first)})));
        QCOMPARE(f.requests.at(1).control->state.load(), SftpFileUploadControl::State::Active);
        f.deliveries.at(1).progress(3, 7);
        QCOMPARE(value(f.query(transferId(second))).value("state").toString(), QString("uploading"));
        const auto crossDirection = StartFileDownloadRequest{upload.reference, upload.localPath, upload.remotePath, upload.operationId, upload.overwrite};
        const auto conflict = f.call(crossDirection); QVERIFY(isError(conflict));
        QCOMPARE(std::get<Error>(conflict).code, ErrorCode::TargetOperationConflict);
    }
    void downloadsAndUploadsShareRetainedQuota()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.downloadGrant(true);
        for (int i = 0; i < 16; ++i) {
            request.operationId = uuid();
            const auto started = i % 2 == 0 ? f.call(request) : f.call(uploadRequest(request));
            QVERIFY(!isError(started));
            SftpFileUploadResult result; result.requestId = f.requests.last().requestId;
            result.published = true; result.publication = "published"; result.temporary = "removed";
            f.deliveries.last().finished(result);
        }
        request.operationId = uuid();
        const auto rejected = f.call(request); QVERIFY(isError(rejected));
        QCOMPARE(std::get<Error>(rejected).code, ErrorCode::Busy); QCOMPARE(f.requests.size(), 16);
    }
    void downloadForgedPrincipalCannotQueryOrCancel()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.downloadGrant();
        const auto started = f.call(request); QVERIFY(!isError(started));
        const auto transfer = transferId(started);
        const auto other = uuid();
        QCOMPARE(f.access.identityVersion(other), quint64(0));
        QVERIFY(isError(f.call(request, other)));
        QVERIFY(isError(f.query(transfer, other)));
        QVERIFY(isError(f.call(CancelFileTransferRequest{transfer}, other)));
        QCOMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Active);
        QVERIFY(!isError(f.call(CancelFileTransferRequest{transfer})));
        QCOMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Cancelled);
    }
    void uploadsWithoutTerminalPermissionAndPreservesPublishedFact()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.grant();
        QVERIFY(f.resources.resources().local(f.client, f.identity()).isEmpty());
        QVERIFY(f.resources.resources().remoteSessions().isEmpty());
        QVERIFY(!f.access.canRead(f.client, f.entry()));
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted); QVERIFY(!transfer.isEmpty());
        QCOMPARE(f.requests.size(), 1); QVERIFY(f.transfers.busy(f.entry().id));
        QCOMPARE(f.requests.first().localRoot, f.root.filePath("build"));
        QCOMPARE(f.requests.first().localRelativePath, QString("firmware.bin"));
        QCOMPARE(f.requests.first().remoteRoot, QString("/tmp/nvt-artifacts"));
        QCOMPARE(f.requests.first().remoteName, QString("firmware-build.bin"));
        const auto initial = value(accepted);
        QVERIFY(initial.value("totalBytes").isNull());
        QCOMPARE(initial.value("sessionId").toString(), request.reference.session.str());
        QCOMPARE(initial.value("epoch").toString(), request.reference.epoch.str());
        QCOMPARE(initial.value("operationId").toString(), request.operationId);
        QVERIFY(!initial.value("published").toBool());
        QVERIFY(!initial.value("contentVerified").toBool(true));
        f.deliveries.first().progress(3, 7);
        auto progress = value(f.query(transfer));
        QCOMPARE(progress.value("transferredBytes").toDouble(), 3.0);
        QCOMPARE(progress.value("totalBytes").toDouble(), 7.0);
        f.requests.first().control->state.store(SftpFileUploadControl::State::AwaitingPublication);
        f.deliveries.first().publicationReady();
        QTRY_COMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Approved);
        f.requests.first().control->state.store(SftpFileUploadControl::State::Publishing);
        f.finish(true);
        const auto completed = value(f.query(transfer));
        QVERIFY(completed.value("published").toBool());
        QCOMPARE(completed.value("publication").toString(), QString("published"));
        QCOMPARE(completed.value("temporary").toString(), QString("removed"));
        QCOMPARE(completed.value("transferredBytes").toDouble(), 7.0);
        QVERIFY(!f.transfers.busy(f.entry().id));
        QVERIFY(!isError(f.call(CancelFileTransferRequest{transfer})));
        QCOMPARE(value(f.query(transfer)), completed);
        f.deliveries.first().progress(1, 7);
        QCOMPARE(value(f.query(transfer)), completed);
    }
    void replayIsReadOnlyAndIgnoresConfirmationFields()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.grant();
        const FileTransfers& index = f.transfers;
        QVERIFY(!index.replay(f.client, f.identity(), request));
        QVERIFY(f.requests.isEmpty());
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        request.confirmationToken = "expired-confirmation"; request.confirmed = false;
#ifdef Q_OS_WIN
        request.localPath = QDir::toNativeSeparators(request.localPath);
        request.localPath[0] = request.localPath[0].toLower();
#endif
        auto replayed = index.replay(f.client, f.identity(), request); QVERIFY(replayed);
        QCOMPARE(value(*replayed), value(accepted)); QCOMPARE(f.requests.size(), 1);
        QVERIFY(!index.replay(f.client, f.identity(), QueryFileTransferRequest{transferId(accepted)}));
        f.finish(true);
        replayed = index.replay(f.client, f.identity(), request); QVERIFY(replayed);
        QVERIFY(value(*replayed).value("published").toBool()); QCOMPARE(f.requests.size(), 1);
        auto changed = request; changed.overwrite = !request.overwrite;
        const auto conflict = index.replay(f.client, f.identity(), changed); QVERIFY(conflict); QVERIFY(isError(*conflict));
        QCOMPARE(std::get<Error>(*conflict).code, ErrorCode::TargetOperationConflict);
        changed = request; changed.localPath += ".other";
        const auto changedPath = index.replay(f.client, f.identity(), changed); QVERIFY(changedPath); QVERIFY(isError(*changedPath));
        QCOMPARE(std::get<Error>(*changedPath).code, ErrorCode::TargetOperationConflict);
        const auto oldIdentity = f.identity(); QVERIFY(f.access.rotateToken());
        const auto denied = index.replay(f.client, oldIdentity, request); QVERIFY(denied); QVERIFY(isError(*denied));
        QCOMPARE(std::get<Error>(*denied).code, ErrorCode::PermissionDenied);
    }
    void replayDetectsDownloadToUploadConflictBeforeConfirmation()
    {
        Fixture f; QVERIFY(f.ready); const auto download = f.downloadGrant();
        const auto accepted = f.call(download); QVERIFY(!isError(accepted));
        const auto upload = uploadRequest(download);
        const auto conflict = f.transfers.replay(f.client, f.identity(), upload);
        QVERIFY(conflict); QVERIFY(isError(*conflict));
        QCOMPARE(std::get<Error>(*conflict).code, ErrorCode::TargetOperationConflict);
        const auto original = f.transfers.replay(f.client, f.identity(), download); QVERIFY(original);
        QCOMPARE(value(*original), value(accepted)); QCOMPARE(f.requests.size(), 1);
    }
    void retriesOperationExactlyOnceAndRejectsChangedArguments()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.grant();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted);
        QCOMPARE(transferId(f.call(request)), transfer);
        QCOMPARE(transferId(f.call(request, {}, "second-connection")), transfer);
        for (int change = 0; change != 5; ++change) {
            auto changed = request;
            switch (change) {
            case 0: changed.localPath += ".other"; break;
            case 1: changed.remotePath += ".other"; break;
            case 2: changed.reference.session = NovaTerm::Mcp::SessionId{uuid()}; break;
            case 3: changed.overwrite = true; break;
            case 4: changed.reference.epoch = Epoch{"old"}; break;
            }
            QVERIFY(isError(f.call(changed)));
        }
        QCOMPARE(f.requests.size(), 1);
        f.finish(true);
        QCOMPARE(transferId(f.call(request)), transfer);
        QCOMPARE(f.requests.size(), 1);
    }
    void cancellationDisconnectAndConnectionChangePreventPublication()
    {
        for (int change = 0; change != 3; ++change) {
            Fixture f; QVERIFY(f.ready); auto request = f.grant();
            const auto accepted = f.call(request); QVERIFY(!isError(accepted));
            const auto transfer = transferId(accepted);
            f.requests.first().control->state.store(SftpFileUploadControl::State::AwaitingPublication);
            if (change == 0) QVERIFY(!isError(f.call(CancelFileTransferRequest{transfer})));
            else if (change == 1) f.transfers.disconnect("connection");
            else f.session.attach(&f.local, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
            f.deliveries.first().publicationReady();
            QTRY_COMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Cancelled);
            QVERIFY(f.transfers.busy(f.entry().id));
            request.operationId = uuid(); QVERIFY(isError(f.call(request)));
            QCOMPARE(f.requests.size(), 1);
            f.finish(false); QVERIFY(!f.transfers.busy(f.entry().id));
            const auto completed = value(f.query(transfer));
            QVERIFY(!completed.value("published").toBool());
            QCOMPARE(completed.value("publication").toString(), QString("not_started"));
            QCOMPARE(completed.value("temporary").toString(), QString("removed"));
        }
    }
    void unknownCompletionPreservesObservedTotal_data()
    {
        QTest::addColumn<int>("observed");
        QTest::newRow("before-first-progress") << 0;
        QTest::newRow("empty-file-known") << 1;
        QTest::newRow("in-progress") << 2;
        QTest::newRow("publication-in-flight") << 3;
    }
    void unknownCompletionPreservesObservedTotal()
    {
        QFETCH(int, observed);
        Fixture f; QVERIFY(f.ready);
        const auto accepted = f.call(f.grant()); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted);
        if (observed == 1) f.deliveries.first().progress(0, 0);
        if (observed >= 2) f.deliveries.first().progress(65536, 131072);
        if (observed == 3) f.requests.first().control->state.store(SftpFileUploadControl::State::Publishing);
        else f.requests.first().control->cancel();
        // The backend-destruction fallback has no final worker byte counters.
        SftpFileUploadResult result;
        result.requestId = f.requests.first().requestId;
        result.publication = observed == 3 ? "unknown" : "not_started";
        result.temporary = "unknown"; result.errorCode = "SESSION_NOT_AVAILABLE";
        f.deliveries.first().finished(result);
        const auto completed = value(f.query(transfer));
        if (observed == 0) QVERIFY(completed.value("totalBytes").isNull());
        else QCOMPARE(completed.value("totalBytes").toDouble(), observed == 1 ? 0.0 : 131072.0);
        QCOMPARE(completed.value("transferredBytes").toDouble(), observed < 2 ? 0.0 : 65536.0);
        QCOMPARE(completed.value("publication").toString(), observed == 3 ? QString("unknown") : QString("not_started"));
        QVERIFY(!completed.value("published").toBool());
        QVERIFY(!f.transfers.busy(f.entry().id));
        f.finish(true); // A late duplicate must not rewrite the settled fact.
        QCOMPARE(value(f.query(transfer)), completed);
    }
    void rejectsForgedPrincipalStaleEpochAndInvalidPaths()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.grant();
        auto stale = request; stale.reference.epoch = Epoch{"old"};
        QVERIFY(isError(f.call(stale)));
        stale = request; stale.localPath = "relative.bin"; QVERIFY(isError(f.call(stale)));
        stale = request; stale.remotePath = "/tmp/../other.bin"; QVERIFY(isError(f.call(stale)));
        const auto other = uuid();
        QCOMPARE(f.access.identityVersion(other), quint64(0));
        QVERIFY(isError(f.call(request, other)));
        QCOMPARE(f.requests.size(), 0);
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto transfer = transferId(accepted);
        const auto denied = f.query(transfer, other); QVERIFY(isError(denied));
        QCOMPARE(std::get<Error>(denied).code, ErrorCode::PermissionDenied);
        QVERIFY(isError(f.call(CancelFileTransferRequest{transfer}, other)));
        QCOMPARE(f.requests.first().control->state.load(), SftpFileUploadControl::State::Active);
        QCOMPARE(f.requests.size(), 1);
    }
};
QTEST_GUILESS_MAIN(FileTransfersTests)
#include "FileTransfersTests.moc"
