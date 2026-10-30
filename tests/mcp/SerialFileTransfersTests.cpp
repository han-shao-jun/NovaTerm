#include "mcp/SerialFileTransfers.h"
#include "core/terminal/TerminalCore.h"
#include "transport/SerialTransport.h"
#include "session/SessionInputArbiter.h"
#include "session/SessionTerminalController.h"
#include "filetransfer/ZmodemEngine.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <QTest>
#include <algorithm>

using namespace NovaTerm::Mcp;
namespace FT = NovaTerm::FileTransfer;
class SerialTransportIoTestAccess {
public:
    static void connected(SerialTransport& transport)
    {
        const auto generation = transport._state->requestOpen();
        transport._state->opened(generation); transport._connected = true;
    }
    static void config(SerialTransport& transport) { transport._config.baudRate = 9600; }
    static void closePending(SerialTransport& transport) { transport._state->requestClose(); }
};
namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject value(const Result<QJsonObject>& result)
{ return std::holds_alternative<QJsonObject>(result) ? std::get<QJsonObject>(result) : QJsonObject{}; }
class LocalTransport final : public ITransport {
public:
    bool connectToHost() override { online = true; emit connected(); return true; }
    void disconnect() override { online = false; emit disconnected(); }
    void write(const QByteArray&) override {}
    void resizeTerminal(int, int) override {}
    bool isConnected() const override { return online; }
    QString errorString() const override { return {}; }
    bool online{false};
};
struct Fixture {
    QTemporaryDir root{QDir::currentPath() + "/novaterm-serial-transfer-XXXXXX"};
    TerminalCore core{80, 24};
    LocalTransport local;
    std::unique_ptr<SerialTransport> serial;
    TerminalSession session{&core};
    AccessStore access{root.filePath("state"), std::make_unique<MemoryCredentialStore>()};
    SessionDirectory directory;
    FileResourceAdapter resources{access, directory, nullptr};
    SerialFileTargetAdapter targets{access, directory, nullptr};
    QByteArray outgoing, allWrites, peerInput, received;
    QString offeredName;
    bool reserved{false}, reserveAllowed{true};
    int starts{0};
    qint64 clock{0};
    std::unique_ptr<SerialFileTransferController> controller;
    SerialFileTransfers transfers{access, directory, resources, targets, "instance", nullptr,
        [this](TerminalSession&) { ++starts; return controller.get(); }, [this] { return clock; }};
    QString client;
    bool ready{false};
    explicit Fixture(int baud = 115200)
    {
        session.attach(&local, TerminalSession::Ownership::Borrowed, TransportKind::LocalShell);
        static_cast<void>(session.start());
        SerialConfig config; config.portName = "COM-private-test"; config.baudRate = baud;
        serial = std::make_unique<SerialTransport>(config);
        SerialTransportIoTestAccess::connected(*serial);
        session.attach(serial.get(), TerminalSession::Ownership::Borrowed, TransportKind::Serial);
        directory.add(&session);
        client = access.ensureAccess();
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [this](quint64) { reserved = reserveAllowed; return reserved; };
        channel.release = [this] { reserved = false; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [this] { return outgoing.size(); };
        channel.clearWrites = [this] { outgoing.clear(); return true; };
        channel.write = [this](QByteArrayView bytes) -> qint64 {
            outgoing.append(bytes); allWrites.append(bytes); return bytes.size();
        };
        channel.baudRate = baud;
        controller = std::make_unique<SerialFileTransferController>(std::move(channel));
        ready = root.isValid() && !client.isEmpty() && access.setEnabled(true);
    }
    ~Fixture() { transfers.stop(); controller.reset(); }
    SessionDirectory::Entry entry() { return directory.entries().first(); }
    quint64 identity() const { return access.identityVersion(client); }
    StartSerialFileUploadRequest source(const QByteArray& contents = "payload")
    {
        QFile file(root.filePath("source.bin"));
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) return {};
        file.close();
        const auto current = entry();
        StartSerialFileUploadRequest request;
        request.reference = {NovaTerm::Mcp::SessionId{current.id}, Epoch{current.epoch}};
        request.localPath = file.fileName(); request.remoteName = "firmware.bin"; request.operationId = uuid();
        return request;
    }
    Result<QJsonObject> call(const NovaTerm::Mcp::SerialFileTransferRequest& request, const QString& owner = {})
    {
        const auto who = owner.isEmpty() ? client : owner;
        return transfers.call(who, access.identityVersion(who), "connection", request);
    }
    QJsonObject query(const QString& id) { return value(call(QuerySerialFileTransferRequest{id})); }
    bool bridge(FT::ZmodemEngine& peer, bool skip = false)
    {
        peerInput.append(outgoing); outgoing.clear(); controller->notifyWritable();
        for (int i = 0; i < 10000; ++i) {
            if (auto action = peer.takeAction()) {
                FT::OperationResult result;
                if (action->kind == FT::ActionKind::OfferFile) {
                    offeredName = QString::fromStdString(action->file.name);
                    result.accepted = !skip;
                }
                if (action->kind == FT::ActionKind::WriteAt) {
                    const auto end = qsizetype(action->offset + action->bytes.size());
                    if (received.size() < end) received.resize(end);
                    std::copy(action->bytes.begin(), action->bytes.end(), received.begin() + qsizetype(action->offset));
                }
                peer.completeOperation(action->id, std::move(result), 0); continue;
            }
            if (peerInput.isEmpty()) break;
            const auto used = peer.consume({peerInput.constData(), peerInput.size()}, 0).consumed;
            if (!used) break;
            peerInput.remove(0, qsizetype(used));
        }
        const auto output = peer.pendingOutput();
        if (!output.empty()) {
            QByteArray bytes(output.data, output.size); peer.acknowledgeOutput(std::size_t(output.size), 0);
            controller->acceptBytes(bytes);
        }
        return !controller->isActive();
    }
};
}
class SerialFileTransfersTests final : public QObject {
    Q_OBJECT
private slots:
    void realProtocolFacts_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("delivered") << 0;
        QTest::newRow("empty-file") << 1;
        QTest::newRow("skipped") << 2;
    }
    void realProtocolFacts()
    {
        QFETCH(int, mode);
        Fixture f; QVERIFY(f.ready);
        QByteArray contents(mode == 1 ? 0 : 16387, 'x');
        const auto request = f.source(contents); QVERIFY(!request.localPath.isEmpty());
        QVERIFY(f.resources.resources().local(f.client, f.identity()).isEmpty());
        QVERIFY(f.targets.targets().list(f.client, f.identity()).isEmpty());
        QVERIFY(!f.access.canRead(f.client, f.entry()));
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString(); QVERIFY(!id.isEmpty());
        QVERIFY(f.transfers.busy(f.entry().id));
        FT::ZmodemEngine peer; FT::TransferRequest receive; receive.direction = FT::Direction::Receive;
        QVERIFY(peer.start(receive, 0));
        QTRY_VERIFY_WITH_TIMEOUT(f.bridge(peer, mode == 2), 10000);
        const auto final = f.query(id);
        QCOMPARE(final.value("state").toString(), mode == 2 ? QString("skipped") : QString("completed"));
        QCOMPARE(final.value("protocol").toString(), QString("zmodem"));
        QCOMPARE(final.value("fileConfirmed").toBool(), mode != 2);
        QVERIFY(final.value("protocolCompleted").toBool());
        QCOMPARE(final.value("completedFiles").toInt(), mode == 2 ? 0 : 1);
        QCOMPARE(final.value("totalBytes").toDouble(), double(contents.size()));
        QCOMPARE(final.value("confirmedBytes").toDouble(), mode == 2 ? 0.0 : double(contents.size()));
        QCOMPARE(final.value("remotePublication").toString(), QString("unknown"));
        QVERIFY(!final.value("contentVerified").toBool(true));
        QCOMPARE(f.received, mode == 2 ? QByteArray{} : contents);
        QVERIFY(!f.transfers.busy(f.entry().id)); QVERIFY(!f.allWrites.contains("rz\r"));
        QCOMPARE(value(f.call(request)), final); QCOMPARE(f.starts, 1);
    }
    void replayIsReadOnlyAndNormalizesDefaultName()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.source(); request.remoteName.clear();
        const SerialFileTransfers& index = f.transfers;
        QVERIFY(!index.replay(f.client, f.identity(), request)); QCOMPARE(f.starts, 0);
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        request.remoteName = "source.bin"; request.confirmationToken = "expired-confirmation"; request.confirmed = false;
#ifdef Q_OS_WIN
        request.localPath = QDir::toNativeSeparators(request.localPath);
        request.localPath[0] = request.localPath[0].toLower();
#endif
        auto replayed = index.replay(f.client, f.identity(), request); QVERIFY(replayed);
        QCOMPARE(value(*replayed), value(accepted)); QCOMPARE(f.starts, 1);
        QVERIFY(!index.replay(f.client, f.identity(), QuerySerialFileTransferRequest{value(accepted).value("transferId").toString()}));
        auto changed = request; changed.remoteName = "other.bin";
        const auto conflict = index.replay(f.client, f.identity(), changed); QVERIFY(conflict); QVERIFY(isError(*conflict));
        QCOMPARE(std::get<Error>(*conflict).code, ErrorCode::TargetOperationConflict);
        SerialTransportIoTestAccess::config(*f.serial);
        replayed = index.replay(f.client, f.identity(), request); QVERIFY(replayed);
        QCOMPARE(value(*replayed), value(accepted)); QVERIFY(f.controller->isActive()); // No admission/cancellation side effect.
        f.controller->abort("fixture terminal fact");
        replayed = index.replay(f.client, f.identity(), request); QVERIFY(replayed);
        QCOMPARE(value(*replayed).value("state").toString(), QString("failed")); QCOMPARE(f.starts, 1);
        const auto oldIdentity = f.identity(); QVERIFY(f.access.rotateToken());
        const auto denied = index.replay(f.client, oldIdentity, request); QVERIFY(denied); QVERIFY(isError(*denied));
        QCOMPARE(std::get<Error>(*denied).code, ErrorCode::PermissionDenied);
    }
    void retriesAndForeignClientsCannotRetarget()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.source();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        QCOMPARE(value(f.call(request)).value("transferId").toString(), id);
        for (int field = 0; field < 4; ++field) {
            auto changed = request;
            if (field == 0) changed.localPath = f.root.filePath("other.bin");
            if (field == 1) changed.remoteName = "other.bin";
            if (field == 2) changed.reference.epoch = Epoch{"old"};
            if (field == 3) changed.reference.session = NovaTerm::Mcp::SessionId{uuid()};
            const auto result = f.call(changed); QVERIFY(isError(result));
            QCOMPARE(std::get<Error>(result).code, ErrorCode::TargetOperationConflict);
        }
        const auto other = uuid();
        QCOMPARE(f.access.identityVersion(other), quint64(0));
        QVERIFY(isError(f.call(QuerySerialFileTransferRequest{id}, other)));
        QVERIFY(isError(f.call(CancelSerialFileTransferRequest{id}, other)));
        QVERIFY(f.controller->isActive()); QCOMPARE(f.starts, 1);
    }
    void identityAndBindingChangesFailClosed_data()
    {
        QTest::addColumn<int>("mode");
        for (int i = 0; i < 5; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void identityAndBindingChangesFailClosed()
    {
        QFETCH(int, mode); Fixture f; QVERIFY(f.ready); auto request = f.source();
        const auto identity = f.identity();
        if (mode == 0) QVERIFY(f.access.setEnabled(false));
        if (mode == 1) QVERIFY(f.access.rotateToken());
        if (mode == 2) request.reference.epoch = Epoch{"old"};
        if (mode == 3) request.reference.session = NovaTerm::Mcp::SessionId{uuid()};
        if (mode == 4) SerialTransportIoTestAccess::closePending(*f.serial);
        QVERIFY(isError(f.transfers.call(f.client, identity, "connection", request)));
        QCOMPARE(f.starts, 0); QVERIFY(f.allWrites.isEmpty());
    }
    void oldRecordCancelCannotStopLaterManualTransfer()
    {
        Fixture f; QVERIFY(f.ready); const auto accepted = f.call(f.source()); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        f.controller->abort("fixture abort");
        const auto before = f.query(id); QVERIFY(!f.transfers.busy(f.entry().id));
        ::SerialTransferRequest manual; manual.files = QStringList{f.root.filePath("source.bin")};
        QVERIFY(f.controller->start(manual));
        QVERIFY(!isError(f.call(CancelSerialFileTransferRequest{id})));
        f.transfers.disconnect("connection"); f.transfers.stop();
        QVERIFY(f.controller->isActive()); QCOMPARE(f.query(id), before);
    }
    void everyTargetBindingComponentMustMatch_data()
    {
        QTest::addColumn<int>("field");
        for (int i = 0; i < 7; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void everyTargetBindingComponentMustMatch()
    {
        QFETCH(int, field); Fixture f; QVERIFY(f.ready);
        const auto live = f.targets.liveBinding(f.entry().id); QVERIFY(live);
        auto binding = *live;
        if (field == 0) binding.connection.session = NovaTerm::Mcp::SessionId{uuid()};
        if (field == 1) binding.connection.attachment = AttachmentId{uuid()};
        if (field == 2) binding.connection.epoch = Epoch{uuid()};
        if (field == 3) binding.connection.generation = Generation{binding.connection.generation.value() + 1};
        if (field == 4) binding.connection.targetFingerprint = "foreign";
        if (field == 5) ++binding.ioGeneration;
        if (field == 6) binding.configDigest = "foreign";
        QVERIFY(!SerialFileTargets::sameBinding(binding, *live));
        QCOMPARE(f.starts, 0); QVERIFY(f.allWrites.isEmpty());
    }
    void deadlineDoesNotMoveOnReplay()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.source();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        QTRY_VERIFY(!f.query(id).value("totalBytes").isNull());
        QCOMPARE(f.query(id).value("timeoutMs").toInt(), 120000);
        f.clock = 119999; QVERIFY(!isError(f.call(request)));
        QVERIFY(f.controller->isActive());
        f.clock = 120000;
        const auto expired = f.query(id);
        QVERIFY(expired.value("state").toString() != "sending");
        QCOMPARE(expired.value("errorCode").toString(), QString("TIMEOUT"));
    }
    void slowBudgetRejectsBeforeAnyProtocolBytes()
    {
        Fixture f(110); QVERIFY(f.ready); const auto request = f.source(QByteArray(32768, 'x'));
        const auto accepted = f.call(request);
        QVERIFY(!isError(accepted)); const auto id = value(accepted).value("transferId").toString();
        QTRY_VERIFY(!f.transfers.busy(f.entry().id));
        QCOMPARE(f.query(id).value("state").toString(), QString("failed"));
        QCOMPARE(f.query(id).value("errorCode").toString(), QString("RESOURCE_LIMIT"));
        QVERIFY(f.allWrites.isEmpty());
        QVERIFY(f.query(id).value("timeoutMs").isNull());
        QVERIFY(value(f.call(request)).value("timeoutMs").isNull());
        QVERIFY(value(f.call(CancelSerialFileTransferRequest{id})).value("timeoutMs").isNull());
    }
    void controllerDestructionPreservesKnownSize()
    {
        Fixture f; QVERIFY(f.ready); const auto accepted = f.call(f.source()); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        QTRY_COMPARE(f.query(id).value("totalBytes").toInt(-1), 7);
        f.controller.reset();
        QCOMPARE(f.query(id).value("totalBytes").toInt(-1), 7);
        QCOMPARE(f.query(id).value("state").toString(), QString("unknown"));
        QVERIFY(!f.transfers.busy(f.entry().id));
    }
    void confirmedFileSurvivesFailedFinalHandshake_data()
    {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("abort") << false;
        QTest::newRow("controller-destroyed") << true;
    }
    void confirmedFileSurvivesFailedFinalHandshake()
    {
        QFETCH(bool, destroy); Fixture f; QVERIFY(f.ready);
        const auto accepted = f.call(f.source()); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        FT::ZmodemEngine peer; FT::TransferRequest receive; receive.direction = FT::Direction::Receive;
        QVERIFY(peer.start(receive, 0));
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            if (f.controller->progress().completedFiles != 1) f.bridge(peer);
            return f.controller->progress().completedFiles == 1;
        })(), 10000);
        QVERIFY(f.controller->isActive());
        if (destroy) f.controller.reset(); else f.controller->abort("final handshake unavailable");
        const auto final = f.query(id);
        QVERIFY(final.value("fileConfirmed").toBool());
        QVERIFY(!final.value("protocolCompleted").toBool());
        QCOMPARE(final.value("confirmedBytes").toInt(), 7);
        QCOMPARE(final.value("completedFiles").toInt(), 1);
        QCOMPARE(final.value("totalBytes").toInt(), 7);
        QCOMPARE(final.value("remotePublication").toString(), QString("unknown"));
    }
    void activeIdentityAndOriginalBindingAreRechecked_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("access-disabled") << 0;
        QTest::newRow("identity-rotated") << 1;
        QTest::newRow("io-replaced") << 2;
        QTest::newRow("config-changed") << 3;
        QTest::newRow("connection-lost") << 4;
    }
    void activeIdentityAndOriginalBindingAreRechecked()
    {
        QFETCH(int, mode); Fixture f; QVERIFY(f.ready); const auto request = f.source();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        QTRY_VERIFY(!f.allWrites.isEmpty());
        const auto before = f.allWrites;
        if (mode == 0) QVERIFY(f.access.setEnabled(false));
        if (mode == 1) QVERIFY(f.access.rotateToken());
        if (mode == 2) SerialTransportIoTestAccess::connected(*f.serial);
        if (mode == 3) SerialTransportIoTestAccess::config(*f.serial);
        if (mode == 4) SerialTransportIoTestAccess::closePending(*f.serial);
        const auto state = f.query(id).value("state").toString();
        QVERIFY(state != "sending" && state != "accepted");
        if (mode < 2) QTRY_VERIFY(!f.controller->isActive());
        if (mode >= 2) {
            QVERIFY(!f.controller->isActive());
            QCOMPARE(f.allWrites, before); // Old binding may not send even cancellation bytes.
        }
    }
    void retainedRecordsExpireWithoutExtendingOnQuery()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.source();
        QString first;
        for (int i = 0; i < 16; ++i) {
            request.operationId = uuid();
            const auto accepted = f.call(request); QVERIFY(!isError(accepted));
            if (first.isEmpty()) first = value(accepted).value("transferId").toString();
            f.controller->abort("retain completion");
        }
        request.operationId = uuid(); QVERIFY(isError(f.call(request)));
        f.clock = 599999; QVERIFY(!f.query(first).isEmpty());
        f.clock = 600000; QVERIFY(f.query(first).isEmpty());
        QVERIFY(!isError(f.call(request)));
    }
    void busyOwnerIsNotPreempted()
    {
        Fixture f; QVERIFY(f.ready); f.reserveAllowed = false;
        const auto accepted = f.call(f.source()); QVERIFY(!isError(accepted));
        const auto id = value(accepted).value("transferId").toString();
        QCOMPARE(f.query(id).value("state").toString(), QString("failed"));
        QVERIFY(f.allWrites.isEmpty()); QVERIFY(!f.reserved);
    }
    void defaultNameAndUnicodeSourceWithoutGrants()
    {
        Fixture f; QVERIFY(f.ready); auto request = f.source("unicode path contents");
        const auto path = f.root.filePath(QString::fromUtf8("固件 build.bin"));
        QVERIFY(QFile::rename(request.localPath, path));
        request.localPath = path; request.remoteName.clear();
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        FT::ZmodemEngine peer; FT::TransferRequest receive; receive.direction = FT::Direction::Receive;
        QVERIFY(peer.start(receive, 0));
        QTRY_VERIFY_WITH_TIMEOUT(f.bridge(peer), 10000);
        QCOMPARE(f.offeredName, QString::fromUtf8("固件 build.bin"));
        QCOMPARE(f.received, QByteArray("unicode path contents"));
        QCOMPARE(f.query(value(accepted).value("transferId").toString()).value("state").toString(), QString("completed"));
    }
    void ownControlIsReleasedAndForeignControlIsPreserved_data()
    {
        QTest::addColumn<bool>("foreign");
        QTest::newRow("own") << false;
        QTest::newRow("foreign") << true;
    }
    void ownControlIsReleasedAndForeignControlIsPreserved()
    {
        QFETCH(bool, foreign); Fixture f; QVERIFY(f.ready); const auto request = f.source();
        auto* terminal = f.session.terminalController(); QVERIFY(terminal);
        const auto owner = foreign ? QString("another-owner") : f.client;
        QVERIFY(terminal->acquire(owner));
        const auto accepted = f.call(request);
        if (foreign) {
            QVERIFY(isError(accepted));
            QCOMPARE(std::get<Error>(accepted).code, ErrorCode::FileTransferBusy);
            QCOMPARE(terminal->owner(), owner); QVERIFY(f.allWrites.isEmpty());
        } else {
            QVERIFY(!isError(accepted)); QVERIFY(terminal->owner().isEmpty());
            QVERIFY(!f.session.inputArbiter()->hasMcpLease());
            FT::ZmodemEngine peer; FT::TransferRequest receive; receive.direction = FT::Direction::Receive;
            QVERIFY(peer.start(receive, 0)); QTRY_VERIFY_WITH_TIMEOUT(f.bridge(peer), 10000);
            QCOMPARE(f.received, QByteArray("payload"));
        }
    }
    void releaseCallbackChangingBindingCannotStartTransfer()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.source();
        auto* terminal = f.session.terminalController(); QVERIFY(terminal->acquire(f.client));
        connect(&f.session, &TerminalSession::terminalControlChanged, &f.session, [&f, terminal] {
            if (terminal->owner().isEmpty()) SerialTransportIoTestAccess::config(*f.serial);
        });
        const auto accepted = f.call(request);
        QVERIFY(isError(accepted)); QVERIFY(!f.controller->isActive()); QVERIFY(f.allWrites.isEmpty());
    }
    void pendingInputCannotBeReleasedForTransfer()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.source();
        auto* terminal = f.session.terminalController(); QVERIFY(terminal->acquire(f.client));
        NovaTerm::TerminalInputAction input; input.kind = NovaTerm::TerminalInputAction::Kind::Text; input.text = "rz\r";
        QVERIFY(terminal->submit(f.client, uuid(), {input}, false)); QVERIFY(terminal->pending());
        const auto accepted = f.call(request); QVERIFY(isError(accepted));
        QCOMPARE(std::get<Error>(accepted).code, ErrorCode::FileTransferBusy);
        QCOMPARE(terminal->owner(), f.client); QVERIFY(f.allWrites.isEmpty());
    }
    void activeReceiverCommandCanHandOffToTransfer()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.source();
        auto* terminal = f.session.terminalController(); QVERIFY(terminal->acquire(f.client));
        InteractiveStreamEvent prompt; prompt.kind = InteractiveStreamEventKind::PromptReady; prompt.promptGeneration = 1;
        terminal->handleEvent(prompt); QVERIFY(terminal->readyForCommand());
        NovaTerm::TerminalInputAction input; input.kind = NovaTerm::TerminalInputAction::Kind::Text; input.text = "rz\r";
        QVERIFY(terminal->submit(f.client, uuid(), {input}, true));
        QTRY_VERIFY(!terminal->pending()); QVERIFY(terminal->commandActive());
        const auto accepted = f.call(request); QVERIFY(!isError(accepted));
        QVERIFY(terminal->owner().isEmpty()); QVERIFY(!terminal->commandActive());
        QCOMPARE(terminal->observation(f.client).value("command").toObject().value("state").toString(), QString("unknown"));
        FT::ZmodemEngine peer; FT::TransferRequest receive; receive.direction = FT::Direction::Receive;
        QVERIFY(peer.start(receive, 0)); QTRY_VERIFY_WITH_TIMEOUT(f.bridge(peer), 10000);
        QCOMPARE(f.received, QByteArray("payload"));
    }
    void defaultSessionOwnerKeepsExistingMcpLease()
    {
        Fixture f; QVERIFY(f.ready); const auto request = f.source();
        auto* arbiter = f.session.inputArbiter(); QVERIFY(arbiter);
        QVERIFY(arbiter->acquireMcpLease(73, f.session.statistics().generation));
        SerialFileTransfers transfers(f.access, f.directory, f.resources, f.targets, "production-owner");
        const auto accepted = transfers.call(f.client, f.identity(), "connection", request);
        QVERIFY(!isError(accepted));
        QCOMPARE(value(accepted).value("state").toString(), QString("failed"));
        QCOMPARE(value(accepted).value("errorCode").toString(), QString("FILE_TRANSFER_BUSY"));
        QVERIFY(arbiter->hasMcpLease()); QVERIFY(!arbiter->hasTransferLease());
        QCOMPARE(f.serial->pendingWriteBytes(), qint64(0));
        arbiter->releaseMcpLease(73);
    }
};
QTEST_GUILESS_MAIN(SerialFileTransfersTests)
#include "SerialFileTransfersTests.moc"
