/** @file SerialFileTransferTests.cpp @brief 串口传输门面的异步与生命周期回归。 */
#include "session/transfer/SerialFileTransferController.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QElapsedTimer>
#include <QPointer>
#include <fstream>
#include "session/transfer/TransferFileWorker.h"
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/ZmodemEngine.h"
namespace FT = NovaTerm::FileTransfer;
class SerialFileTransferTests : public QObject {
    Q_OBJECT
private slots:
    void peerRoundTrip_data() {
        QTest::addColumn<int>("protocol"); QTest::addColumn<bool>("sending");
        QTest::addColumn<bool>("empty"); QTest::addColumn<bool>("batch");
        for (int protocol = 0; protocol != 5; ++protocol) for (const bool sending : {false, true})
            QTest::newRow(qPrintable(QString::number(protocol) + (sending ? "-send" : "-receive"))) << protocol << sending << false << false;
        for (const int protocol : {1, 3, 4}) for (const bool sending : {false, true})
            QTest::newRow(qPrintable(QString::number(protocol) + (sending ? "-empty-send" : "-empty-receive"))) << protocol << sending << true << false;
        for (const int protocol : {3, 4}) for (const bool sending : {false, true})
            QTest::newRow(qPrintable(QString::number(protocol) + (sending ? "-batch-send" : "-batch-receive"))) << protocol << sending << false << true;
    }
    void peerRoundTrip() {
        QFETCH(int, protocol); QFETCH(bool, sending); QFETCH(bool, empty); QFETCH(bool, batch);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto source = directory.filePath("source.bin");
        QByteArray contents(4097, Qt::Uninitialized);
        for (qsizetype n = 0; n < contents.size(); ++n) contents[n] = char(n % 256);
        contents.append(QByteArray("\x1a\0", 2));
        if (empty) contents.clear();
        if (sending) { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(contents), contents.size()); }
        QByteArray transport, peerInput, received, visible;
        bool reserved = false, activated = false; qint64 maxPending = 0;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.activate = [&] { activated = true; };
        channel.release = [&] { reserved = false; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [&] { return transport.size(); };
        channel.clearWrites = [&] { transport.clear(); return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 {
            const auto count = std::min<qsizetype>(23, bytes.size());
            transport.append(bytes.first(count)); maxPending = std::max<qint64>(maxPending, transport.size()); return count;
        };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QObject::connect(&controller, &SerialFileTransferController::visibleRemainder, &controller,
                         [&](const QByteArray& bytes) { visible.append(bytes); });
        std::unique_ptr<FT::ITransferEngine> peer;
        if (protocol == 3) peer = std::make_unique<FT::YmodemEngine>();
        else if (protocol == 4) peer = std::make_unique<FT::ZmodemEngine>();
        else peer = std::make_unique<FT::XmodemEngine>();
        FT::TransferRequest other;
        other.direction = sending ? FT::Direction::Receive : FT::Direction::Send;
        other.files = {{"received.bin", quint64(contents.size())}}; other.expectedSize = contents.size();
        if (protocol == 0) other.config.xmodemMode = FT::XmodemMode::Checksum;
        if (protocol == 2) other.config.xmodemMode = FT::XmodemMode::OneK;
        QVERIFY(peer->start(other, 0));
        if (batch) other.files.push_back({"empty.bin", 0});
        // start 已调用，批次请求需用完整元信息重新创建同协议引擎。
        if (batch) {
            if (protocol == 3) peer = std::make_unique<FT::YmodemEngine>();
            else peer = std::make_unique<FT::ZmodemEngine>();
            QVERIFY(peer->start(other, 0));
        }
        SerialTransferRequest request; request.protocol = SerialTransferProtocol(protocol);
        request.direction = sending ? FT::Direction::Send : FT::Direction::Receive;
        if (sending) {
            request.files = {source};
            if (batch) { QFile emptyFile(directory.filePath("empty.bin")); QVERIFY(emptyFile.open(QIODevice::WriteOnly)); emptyFile.close(); request.files.append(emptyFile.fileName()); }
        }
        else { request.destination = protocol <= 2 ? directory.filePath("received.bin") : directory.path(); request.expectedSize = contents.size(); }
        QVERIFY(controller.start(request));
        QElapsedTimer elapsed; elapsed.start(); QTimer bridge; bridge.setInterval(1);
        bool promptSent = false;
        QObject::connect(&bridge, &QTimer::timeout, &controller, [&] {
            if (!activated || !controller.isActive()) return;
            const auto now = FT::TimePoint(elapsed.elapsed());
            const auto drain = std::min<qsizetype>(37, transport.size());
            peerInput.append(transport.first(drain)); transport.remove(0, drain);
            controller.notifyWritable();
            for (int n = 0; n < 10000; ++n) {
                if (auto action = peer->takeAction()) {
                    FT::OperationResult result;
                    if (action->kind == FT::ActionKind::ReadAt) {
                        const auto bytes = contents.mid(qsizetype(action->offset), qsizetype(action->count));
                        result.bytes.assign(reinterpret_cast<const quint8*>(bytes.constData()), reinterpret_cast<const quint8*>(bytes.constData()) + bytes.size());
                    } else if (action->kind == FT::ActionKind::WriteAt) {
                        const auto end = qsizetype(action->offset + action->bytes.size());
                        if (received.size() < end) received.resize(end);
                        std::copy(action->bytes.begin(), action->bytes.end(), received.begin() + qsizetype(action->offset));
                    }
                    peer->completeOperation(action->id, std::move(result), now); continue;
                }
                if (peerInput.isEmpty()) break;
                const auto consumed = peer->consume({peerInput.constData(), peerInput.size()}, now).consumed;
                if (!consumed) break;
                peerInput.remove(0, qsizetype(consumed));
            }
            peer->advance(now);
            auto output = peer->pendingOutput();
            if (!output.empty()) {
                QByteArray bytes(output.data, output.size);
                peer->acknowledgeOutput(std::size_t(output.size), now);
                if (!sending && protocol == 4 && peer->progress().state == FT::State::Completed) {
                    bytes.append("device> "); promptSent = true;
                }
                controller.acceptBytes(bytes);
            }
            if (!sending && !promptSent && (peer->progress().state == FT::State::Completed || peer->progress().state == FT::State::Closing)) {
                promptSent = true; controller.acceptBytes(QByteArray("device> "));
            }
        });
        bridge.start(); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000); bridge.stop();
        QVERIFY2(done.at(0).at(0).toBool(), qPrintable(done.at(0).at(1).toString()));
        QVERIFY(!reserved); QVERIFY(maxPending <= 16 * 1024);
        QCOMPARE(controller.progress().completedFiles, quint64(batch ? 2 : 1));
        QVERIFY(controller.progress().totalBytes.has_value()); QCOMPARE(*controller.progress().totalBytes, quint64(contents.size()));
        if (sending) QCOMPARE(received, contents);
        else { QFile file(directory.filePath("received.bin")); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), contents); QCOMPARE(visible, QByteArray("device> "));
            if (batch) { QFile emptyFile(directory.filePath("empty.bin")); QVERIFY(emptyFile.open(QIODevice::ReadOnly)); QVERIFY(emptyFile.readAll().isEmpty()); }
        }
    }
    void atomicPublicationDoesNotOverwriteRacingTarget() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::Ymodem;
        request.direction = FT::Direction::Receive; request.destination = directory.path();
        QPointer<TransferFileWorker> worker = new TransferFileWorker(7, request);
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        QSignalSpy results(worker, &TransferFileWorker::resultReady);
        worker->start(); QTRY_COMPARE(results.size(), 1);
        FT::TransferAction offer; offer.id = 1; offer.kind = FT::ActionKind::OfferFile; offer.file = {"target.bin", 0};
        QVERIFY(worker->submit(offer)); QTRY_COMPARE(results.size(), 2);
        QVERIFY(qvariant_cast<TransferFileResult>(results.at(1).at(0)).result.success);
        QFile racingTarget(directory.filePath("target.bin")); QVERIFY(racingTarget.open(QIODevice::WriteOnly)); racingTarget.write("keep"); racingTarget.close();
        FT::TransferAction commit; commit.id = 2; commit.kind = FT::ActionKind::FinishFile;
        QVERIFY(worker->submit(commit)); QTRY_COMPARE(results.size(), 3);
        QVERIFY(!qvariant_cast<TransferFileResult>(results.at(2).at(0)).result.success);
        worker->requestStop(); QTRY_VERIFY(worker.isNull());
        QVERIFY(racingTarget.open(QIODevice::ReadOnly)); QCOMPARE(racingTarget.readAll(), QByteArray("keep"));
        QCOMPARE(QDir(directory.path()).entryList({".novaterm-*"}, QDir::Files | QDir::Hidden).size(), 0);
    }
    void unsafeReceiveNames_data() {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"../escape", "/absolute", "CON.txt", "COM1", "name.", "dir/file", "..", "nul", "a:b"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void unsafeReceiveNames() {
        QFETCH(QString, name); QTemporaryDir directory;
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::Ymodem;
        request.direction = FT::Direction::Receive; request.destination = directory.path();
        QPointer<TransferFileWorker> worker = new TransferFileWorker(1, request);
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        QSignalSpy results(worker, &TransferFileWorker::resultReady);
        worker->start(); QTRY_COMPARE(results.size(), 1);
        FT::TransferAction offer; offer.id = 1; offer.kind = FT::ActionKind::OfferFile; offer.file.name = name.toUtf8().toStdString();
        QVERIFY(worker->submit(offer)); QTRY_COMPARE(results.size(), 2);
        QVERIFY(!qvariant_cast<TransferFileResult>(results.at(1).at(0)).result.success);
        worker->requestStop(); QTRY_VERIFY(worker.isNull());
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }
    void cancellationInsidePublicationRollsBackOnlyOwnFile_data() {
        QTest::addColumn<int>("replacement");
        QTest::newRow("own-link") << 0;
        QTest::newRow("foreign-regular-file") << 1;
        QTest::newRow("foreign-replacement-after-identity-check") << 3;
        QTest::newRow("new-target-blocks-foreign-restore") << 4;
#ifndef Q_OS_WIN
        QTest::newRow("foreign-symbolic-link") << 2;
#endif
    }
    void cancellationInsidePublicationRollsBackOnlyOwnFile() {
        QFETCH(int, replacement); QTemporaryDir directory; QVERIFY(directory.isValid());
        QFile foreign(directory.filePath("foreign.txt")); QVERIFY(foreign.open(QIODevice::WriteOnly)); foreign.write("foreign"); foreign.close();
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::Ymodem;
        request.direction = FT::Direction::Receive; request.destination = directory.path();
        TransferFileWorker* stopInsideLink = nullptr; int publications = 0;
        auto link = [&](const std::filesystem::path& source, const std::filesystem::path& target, std::error_code& error) {
            ++publications;
            if (publications == 2) stopInsideLink->requestStop(); // 真实系统调用之前取消。
            std::filesystem::create_hard_link(source, target, error);
            if (publications == 2 && !error && (replacement == 1 || replacement == 2)) {
                std::filesystem::remove(target, error);
                if (replacement == 1) { std::ofstream file(target); file << "foreign"; }
                else std::filesystem::create_symlink(target.parent_path() / "foreign.txt", target, error);
            }
        };
        auto move = [&](const std::filesystem::path& source, const std::filesystem::path& target, std::error_code& error) {
            if (replacement >= 3) {
                std::filesystem::remove(source, error);
                { std::ofstream file(source); file << "foreign"; }
            }
#ifdef Q_OS_WIN
            std::filesystem::remove(target, error); // 测试隔离文件占位，不模拟外部竞争。
#endif
            std::filesystem::rename(source, target, error);
            if (replacement == 4 && !error) { std::ofstream file(source); file << "newer"; }
        };
        TransferFileWorker::LinkOperation moveHook;
        if (replacement >= 3) moveHook = move;
        QPointer<TransferFileWorker> worker = new TransferFileWorker(9, request, link, std::move(moveHook)); stopInsideLink = worker;
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        QSignalSpy results(worker, &TransferFileWorker::resultReady); worker->start(); QTRY_COMPARE(results.size(), 1);
        FT::TransferAction action; action.id = 1; action.kind = FT::ActionKind::OfferFile; action.file = {"completed.bin", 0};
        QVERIFY(worker->submit(action)); QTRY_COMPARE(results.size(), 2);
        action.id = 2; action.kind = FT::ActionKind::FinishFile; QVERIFY(worker->submit(action)); QTRY_COMPARE(results.size(), 3);
        QVERIFY(qvariant_cast<TransferFileResult>(results.at(2).at(0)).result.success);
        action.id = 3; action.kind = FT::ActionKind::OfferFile; action.file = {"cancelled.bin", 0};
        QVERIFY(worker->submit(action)); QTRY_COMPARE(results.size(), 4);
        action.id = 4; action.kind = FT::ActionKind::FinishFile; QVERIFY(worker->submit(action)); QTRY_VERIFY(worker.isNull());
        QCOMPARE(results.size(), 4); QVERIFY(QFileInfo::exists(directory.filePath("completed.bin")));
        QFile cancelled(directory.filePath("cancelled.bin"));
        if (!replacement) QVERIFY(!cancelled.exists());
        else { QVERIFY(cancelled.open(QIODevice::ReadOnly)); QCOMPARE(cancelled.readAll(), QByteArray(replacement == 4 ? "newer" : "foreign")); }
        QVERIFY(foreign.open(QIODevice::ReadOnly)); QCOMPARE(foreign.readAll(), QByteArray("foreign"));
        const auto leftovers = QDir(directory.path()).entryList({".novaterm-*"}, QDir::Files | QDir::Hidden);
        QCOMPARE(leftovers.size(), replacement == 4 ? 1 : 0);
        if (replacement == 4) {
            QFile preserved(directory.filePath(leftovers.first())); QVERIFY(preserved.open(QIODevice::ReadOnly));
            QCOMPARE(preserved.readAll(), QByteArray("foreign"));
        }
    }
    void stalledHostOutputTimesOut_data() {
        QTest::addColumn<bool>("acceptOutput"); QTest::addColumn<bool>("spuriousNotifications");
        QTest::newRow("accepted-but-not-drained") << true << false;
        QTest::newRow("write-zero-without-notification") << false << false;
        QTest::newRow("notifications-without-drain") << true << true;
    }
    void stalledHostOutputTimesOut() {
        QFETCH(bool, acceptOutput); QFETCH(bool, spuriousNotifications); QTemporaryDir directory;
        QFile file(directory.filePath("source")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x"); file.close();
        qint64 pending = 0; int activated = 0; int attempts = 0; bool reserved = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; }; channel.activate = [&] { ++activated; };
        channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [&] { return pending; };
        channel.clearWrites = [&] { pending = 0; return true; }; channel.writeDrainTimeoutMs = 150;
        channel.write = [&](QByteArrayView bytes) -> qint64 {
            if (quint8(bytes.at(0)) == 0x18) return bytes.size();
            ++attempts; if (acceptOutput) { pending += bytes.size(); return bytes.size(); }
            return 0;
        };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc; request.files = {file.fileName()};
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1); controller.acceptBytes("C");
        QTRY_VERIFY(attempts > 0);
        QTimer notifications; notifications.setInterval(5);
        QObject::connect(&notifications, &QTimer::timeout, &controller, &SerialFileTransferController::notifyWritable);
        if (spuriousNotifications) notifications.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1000); notifications.stop();
        QVERIFY(!done.at(0).at(0).toBool()); QCOMPARE(disconnected.size(), 1); QVERIFY(!reserved); QVERIFY(!controller.isActive());
        QVERIFY(attempts <= 10); // 无进展时使用有界间隔，不做零延迟 GUI 自旋。
    }
    void cancellationDuringClosingDoesNotBecomeVisibleText() {
        QTemporaryDir directory; QByteArray output, visible;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; }; channel.release = [] {};
        channel.activate = [] {}; channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [] { return 0; };
        channel.clearWrites = [] { return true; }; channel.write = [&](QByteArrayView bytes) { output.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        QObject::connect(&controller, &SerialFileTransferController::visibleRemainder, &controller, [&](const QByteArray& bytes) { visible += bytes; });
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc;
        request.direction = FT::Direction::Receive; request.destination = directory.filePath("empty"); request.expectedSize = 0;
        QVERIFY(controller.start(request)); QTRY_VERIFY(output.contains('C'));
        controller.acceptBytes(QByteArray(1, char(4))); QTRY_VERIFY(output.contains(char(6)));
        controller.acceptBytes(QByteArray(2, char(0x18)));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000); QVERIFY(!done.at(0).at(0).toBool());
        QVERIFY(visible.isEmpty()); QCOMPARE(disconnected.size(), 1);
    }
    void inputBudgetAndResumeOrdering() {
        QTemporaryDir directory; QFile file(directory.filePath("source")); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; };
        channel.release = [] {}; channel.coreIdle = [] { return false; }; channel.pendingWriteBytes = [] { return 0; };
        SerialFileTransferController controller(channel); QSignalSpy pauses(&controller, &SerialFileTransferController::readPauseChanged);
        QObject::connect(&controller, &SerialFileTransferController::readPauseChanged, &controller, [&](bool paused) {
            if (!paused) QVERIFY(!controller.isActive());
        });
        SerialTransferRequest request; request.files = {file.fileName()}; QVERIFY(controller.start(request));
        controller.acceptBytes(QByteArray(192 * 1024, 'C')); QCOMPARE(pauses.size(), 1); QVERIFY(pauses.at(0).at(0).toBool());
        controller.acceptBytes(QByteArray(65 * 1024, 'C')); QVERIFY(!controller.isActive());
        QCOMPARE(pauses.size(), 2); QVERIFY(!pauses.at(1).at(0).toBool()); QVERIFY(!controller.errorString().isEmpty());
    }
    void asynchronousShortReadFailsAndDisconnects() {
        QTemporaryDir directory; const auto source = directory.filePath("source");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QByteArray(128, 'x')); file.close();
        int activated = 0; QByteArray output;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; }; channel.release = [] {};
        channel.activate = [&] { ++activated; }; channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [] { return 0; };
        channel.clearWrites = [] { return true; }; channel.write = [&](QByteArrayView bytes) { output.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc; request.files = {source};
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.close(); controller.acceptBytes("C");
        QTRY_COMPARE(done.size(), 1); QVERIFY(!done.at(0).at(0).toBool()); QCOMPARE(disconnected.size(), 1);
        QCOMPARE(output, QByteArray(5, char(0x18)));
    }
    void destructionIgnoresLateWorkerResults() {
        QTemporaryDir directory; QFile file(directory.filePath("source")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("data"); file.close();
        int writes = 0;
        for (int cycle = 0; cycle != 25; ++cycle) {
            SerialFileTransferController::Channel channel;
            channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; }; channel.release = [] {};
            channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [] { return 0; };
            channel.write = [&](QByteArrayView bytes) { ++writes; return bytes.size(); };
            auto controller = std::make_unique<SerialFileTransferController>(channel);
            SerialTransferRequest request; request.files = {file.fileName()}; QVERIFY(controller->start(request));
        }
        QTest::qWait(150); QCOMPARE(writes, 0);
    }
    void actualDrainGatesAckAndCancelDisconnects() {
        QTemporaryDir directory; const auto source = directory.filePath("source");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(QByteArray(128, 'x')), qint64(128)); file.close();
        QByteArray transport; int activations = 0;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; };
        channel.activate = [&] { ++activations; }; channel.release = [] {};
        channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [&] { return transport.size(); };
        channel.clearWrites = [&] { transport.clear(); return true; };
        channel.write = [&](QByteArrayView bytes) { transport.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel); QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc; request.files = {source};
        QVERIFY(controller.start(request)); QTRY_COMPARE(activations, 1);
        controller.acceptBytes("C"); QTRY_COMPARE(transport.size(), 133);
        transport.remove(0, 30); controller.notifyWritable(); controller.acceptBytes(QByteArray(1, char(6)));
        QCOMPARE(controller.progress().transferredBytes, quint64(0)); QCOMPARE(transport.size(), 103);
        transport.clear(); controller.acceptBytes(QByteArray(1, char(6)));
        QTRY_COMPARE(controller.progress().transferredBytes, quint64(128));
        QCOMPARE(transport, QByteArray(1, char(4)));
        controller.cancel(); QVERIFY(controller.isActive()); QCOMPARE(transport, QByteArray(5, char(0x18)));
        transport.clear(); controller.notifyWritable(); QCOMPARE(disconnected.size(), 1); QVERIFY(!controller.isActive());
        // 迟到写入通知不能改变结束状态或向旧串口写入。
        controller.notifyWritable(); QCOMPARE(disconnected.size(), 1); QVERIFY(transport.isEmpty());
    }
    void queuedCoreBarrierWaitsForPriorWrite() {
        QTemporaryDir directory; const auto source = directory.filePath("source");
        QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        bool queued = false; qint64 pending = 0; int activations = 0;
        SerialFileTransferController* pointer = nullptr;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; };
        channel.release = [] {}; channel.activate = [&] { ++activations; };
        channel.coreIdle = [&] {
            if (!queued) { queued = true; QMetaObject::invokeMethod(pointer, [&] { pending = 7; }, Qt::QueuedConnection); }
            return true;
        };
        channel.pendingWriteBytes = [&] { return pending; };
        channel.write = [](QByteArrayView bytes) { return bytes.size(); };
        SerialFileTransferController controller(channel); pointer = &controller;
        SerialTransferRequest request; request.files = {source}; QVERIFY(controller.start(request));
        QTRY_COMPARE(pending, qint64(7)); QTest::qWait(50); QCOMPARE(activations, 0);
        pending = 0; controller.notifyWritable(); QTRY_COMPARE(activations, 1); controller.abort("done");
    }
    void cancelledPreparationCannotActivateNewGeneration() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto source = directory.filePath("source"); QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("old"); file.close();
        int activations = 0; int writes = 0;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; };
        channel.release = [] {}; channel.activate = [&] { ++activations; };
        channel.coreIdle = [] { return false; }; channel.pendingWriteBytes = [] { return 0; };
        channel.write = [&](QByteArrayView data) { ++writes; return data.size(); };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        SerialTransferRequest request; request.files = {source};
        QVERIFY(controller.start(request)); controller.cancel(); QVERIFY(!controller.isActive());
        QVERIFY(controller.start(request)); controller.abort("disconnect");
        QTest::qWait(100); QCOMPARE(done.size(), 2); QCOMPARE(activations, 0); QCOMPARE(writes, 0);
    }
    void existingReceiveTargetIsPreserved() {
        QTemporaryDir directory; const auto target = directory.filePath("target");
        QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("keep"); file.close();
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; };
        channel.release = [] {}; channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [] { return 0; };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc;
        request.direction = FT::Direction::Receive; request.destination = target;
        QVERIFY(controller.start(request)); QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done.at(0).at(0).toBool()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("keep"));
    }

    void preparationFailureReleasesLease() {
        bool reserved = false; int activations = 0; QByteArray written;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.activate = [&] { ++activations; };
        channel.release = [&] { reserved = false; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [] { return 0; };
        channel.write = [&](QByteArrayView bytes) { written.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy finished(&controller, &SerialFileTransferController::finished);
        SerialTransferRequest request; request.files = {QStringLiteral("/missing/novaterm-source")};
        QVERIFY(controller.start(request)); QVERIFY(controller.isActive());
        QTRY_COMPARE(finished.size(), 1); QVERIFY(!controller.isActive());
        QVERIFY(!reserved); QCOMPARE(activations, 0); QVERIFY(written.isEmpty());
    }
    void reserveFailureDoesNotStart() {
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [](quint64) { return false; };
        SerialFileTransferController controller(channel); SerialTransferRequest request;
        request.files = {QStringLiteral("any")}; QVERIFY(!controller.start(request));
        QVERIFY(!controller.isActive()); QVERIFY(!controller.errorString().isEmpty());
    }
};
QTEST_GUILESS_MAIN(SerialFileTransferTests)
#include "SerialFileTransferTests.moc"
