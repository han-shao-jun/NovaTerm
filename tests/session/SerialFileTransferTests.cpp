/** @file SerialFileTransferTests.cpp @brief 串口传输门面的异步与生命周期回归。 */
#include "session/transfer/SerialFileTransferController.h"
#include "transport/SerialIoState.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QThread>
#include <QSemaphore>
#include <atomic>
#include <mutex>
#include <fstream>
#include "session/transfer/TransferFileWorker.h"
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/ZmodemEngine.h"
namespace FT = NovaTerm::FileTransfer;
namespace {
struct SourceTrace {
    QByteArray contents;
    std::optional<quint64> reportedSize;
    QThread* guiThread{QThread::currentThread()};
    std::atomic<QThread*> fileThread{nullptr};
    std::atomic_int opened{0}, reads{0}, closed{0};
    std::atomic_bool wrongThread{false};
    bool failOpen{false}, failRead{false}, shortRead{false};
    bool blockOpen{false}, blockRead{false};
    QSemaphore entered, resume;
    std::mutex mutex;
    std::vector<std::pair<quint64, qsizetype>> ranges;
};
class MemorySerialSource final : public SerialTransferSource {
public:
    explicit MemorySerialSource(std::shared_ptr<SourceTrace> trace) : _trace(std::move(trace)) {}
    ~MemorySerialSource() override {
        checkThread();
        ++_trace->closed;
    }
    quint64 size() const noexcept override {
        return _trace->reportedSize.value_or(quint64(_trace->contents.size()));
    }
    bool readAt(quint64 offset, qsizetype count, QByteArray& bytes, QString& error) override {
        checkThread();
        ++_trace->reads;
        {
            std::lock_guard<std::mutex> lock(_trace->mutex);
            _trace->ranges.emplace_back(offset, count);
        }
        if (_trace->blockRead) { _trace->entered.release(); _trace->resume.acquire(); }
        if (_trace->failRead) { error = QStringLiteral("controlled read failure"); return false; }
        if (count <= 0 || offset > quint64(_trace->contents.size())
            || quint64(count) > quint64(_trace->contents.size()) - offset) {
            error = QStringLiteral("invalid source range"); return false;
        }
        bytes = _trace->contents.mid(qsizetype(offset), count - (_trace->shortRead ? 1 : 0));
        return true;
    }
private:
    void checkThread() const {
        if (QThread::currentThread() != _trace->fileThread.load()
            || QThread::currentThread() == _trace->guiThread) _trace->wrongThread = true;
    }
    std::shared_ptr<SourceTrace> _trace;
};
SerialTransferRequest sourceRequest(const std::shared_ptr<SourceTrace>& trace)
{
    SerialTransferRequest request;
    request.source = SerialTransferSourceSpec{QStringLiteral("固件.bin"),
        [trace](QString& error) -> std::unique_ptr<SerialTransferSource> {
            trace->fileThread = QThread::currentThread();
            if (QThread::currentThread() == trace->guiThread) trace->wrongThread = true;
            ++trace->opened;
            if (trace->blockOpen) { trace->entered.release(); trace->resume.acquire(); }
            if (trace->failOpen) { error = QStringLiteral("controlled open failure"); return {}; }
            return std::make_unique<MemorySerialSource>(trace);
        }};
    return request;
}
/** @brief 测试提前返回也释放受控文件动作，避免遗留阻塞线程。 */
struct SourceGateRelease {
    std::shared_ptr<SourceTrace> trace;
    ~SourceGateRelease() { trace->resume.release(); }
};
struct SourceChannel {
    QByteArray output;
    bool reserved{false}, idle{true};
    int activations{0}, releases{0};
    SerialFileTransferController::Channel channel() {
        SerialFileTransferController::Channel result;
        result.connected = [] { return true; };
        result.reserve = [this](quint64) { if (reserved) return false; reserved = true; return true; };
        result.release = [this] { reserved = false; ++releases; };
        result.activate = [this] { ++activations; };
        result.coreIdle = [this] { return idle; };
        result.pendingWriteBytes = [] { return 0; };
        result.clearWrites = [] { return true; };
        result.write = [this](QByteArrayView bytes) { output.append(bytes); return bytes.size(); };
        return result;
    }
};
void sourcePeerHeader(SerialFileTransferController& controller, FT::ZFrame frame, quint32 position = 0)
{
    const auto bytes = FT::ZmodemCodec::header(frame, position, FT::ZHeaderFormat::Hex);
    controller.acceptBytes(QByteArray(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size())));
}
std::optional<QByteArray> sourcePayload(const QByteArray& wire)
{
    FT::ZmodemCodec codec;
    for (const auto byte : wire) {
        if (const auto event = codec.feed(quint8(byte))) {
            if (event->kind == FT::ZEventKind::Header && event->frame == FT::ZFrame::Data)
                codec.expectData(event->crc32, 8192);
            else if (event->kind == FT::ZEventKind::Data)
                return QByteArray(reinterpret_cast<const char*>(event->bytes.data()), qsizetype(event->bytes.size()));
        }
    }
    return {};
}
}
class SerialFileTransferTests : public QObject {
    Q_OBJECT
private slots:
    void synchronousStartDeletionDoesNotOpenSource() {
        SourceChannel channel;
        QPointer<SerialFileTransferController> controller = new SerialFileTransferController(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        connect(controller, &SerialFileTransferController::activeChanged, this, [&](bool active) {
            if (active) delete controller.data();
        });
        auto* original = controller.data();
        QVERIFY(original->start(sourceRequest(trace)));
        QVERIFY(controller.isNull());
        QTest::qWait(30);
        QCOMPARE(trace->opened.load(), 0);
        QVERIFY(!channel.reserved);
    }
    void acceptedIdentityPrecedesSynchronousNotification() {
        SourceChannel channel;
        SerialFileTransferController controller(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        quint64 id = 0, notifiedId = 0;
        connect(&controller, &SerialFileTransferController::activeChanged, &controller, [&](bool active) {
            if (active) notifiedId = id;
        });
        QVERIFY(controller.start(sourceRequest(trace), &id));
        QCOMPARE(notifiedId, controller.progress().transferId);
        QVERIFY(id != 0);
        controller.abort(QStringLiteral("done"));
    }
    void staleIdentityCannotCancelOrAbortLaterTask() {
        SourceChannel channel;
        SerialFileTransferController controller(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        QVERIFY(controller.start(sourceRequest(trace)));
        const auto oldId = controller.progress().transferId;
        controller.abort(QStringLiteral("old"));
        QVERIFY(controller.start(sourceRequest(trace)));
        QVERIFY(!controller.cancel(oldId));
        QVERIFY(!controller.abort(oldId, QStringLiteral("stale")));
        QVERIFY(controller.isActive());
        controller.abort(QStringLiteral("done"));
    }
    void finishRejectsSynchronousRestartAndPreservesResult() {
        SourceChannel channel;
        SerialFileTransferController controller(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        const auto request = sourceRequest(trace);
        QSignalSpy result(&controller, &SerialFileTransferController::resultReady);
        bool restarted = false;
        connect(&controller, &SerialFileTransferController::activeChanged, &controller, [&](bool active) {
            if (!active) restarted = controller.start(request);
        });
        QVERIFY(controller.start(request));
        const auto id = controller.progress().transferId;
        controller.abort(QStringLiteral("original"));
        const bool wasActive = controller.isActive();
        disconnect(&controller, nullptr, &controller, nullptr);
        controller.abort(QStringLiteral("cleanup"));
        QVERIFY(!restarted); QVERIFY(!wasActive);
        QCOMPARE(result.size(), 1);
        const auto completed = qvariant_cast<SerialTransferResult>(result.front().front());
        QCOMPARE(completed.transferId, id); QCOMPARE(completed.progress.transferId, id);
        QVERIFY(!completed.protocolCompleted); QVERIFY(!completed.progress.active);
        QCOMPARE(completed.progress.status, QStringLiteral("original"));
    }
    void synchronousStartAbortDoesNotOpenSource() {
        SourceChannel channel;
        SerialFileTransferController controller(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        connect(&controller, &SerialFileTransferController::activeChanged, &controller, [&](bool active) {
            if (active) controller.abort(QStringLiteral("revoked"));
        });
        QVERIFY(controller.start(sourceRequest(trace)));
        QTest::qWait(50);
        QCOMPARE(trace->opened.load(), 0);
        QVERIFY(!controller.isActive());
    }
    void admissionBeforePreparationRejectsSourceAccess() {
        SourceChannel channel;
        SerialFileTransferController controller(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        auto request = sourceRequest(trace); request.admission = [] { return false; };
        quint64 id = 99;
        QVERIFY(!controller.start(request, &id));
        QCOMPARE(id, quint64(0)); QCOMPARE(trace->opened.load(), 0);
        QVERIFY(channel.output.isEmpty()); QVERIFY(!channel.reserved);
    }
    void admissionRevokedDuringPreparationPreventsProtocolOutput() {
        SourceChannel channel; channel.idle = false;
        SerialFileTransferController controller(channel.channel());
        auto trace = std::make_shared<SourceTrace>();
        bool allowed = true;
        auto request = sourceRequest(trace); request.admission = [&] { return allowed; };
        QVERIFY(controller.start(request));
        QTRY_VERIFY(controller.progress().totalBytes.has_value());
        allowed = false; channel.idle = true;
        controller.notifyWritable();
        QTest::qWait(30);
        QCOMPARE(channel.activations, 0); QVERIFY(channel.output.isEmpty());
        QVERIFY(!controller.isActive());
    }
    void sourceSpecRejectsAmbiguousRequests_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<QString>("name");
        for (const auto& name : QStringList{"", ".", "..", "../firmware", "dir/file", "dir\\file",
                "CON.bin", "file:stream", "file.", "file ", "file\n", QString(256, 'a')})
            QTest::newRow(qPrintable("name-" + name)) << 0 << name;
        for (int kind = 1; kind <= 6; ++kind)
            QTest::newRow(qPrintable(QStringLiteral("mixed-%1").arg(kind))) << kind << QStringLiteral("source.bin");
    }
    void sourceSpecRejectsAmbiguousRequests() {
        QFETCH(int, kind); QFETCH(QString, name);
        auto trace = std::make_shared<SourceTrace>();
        auto request = sourceRequest(trace); request.source->name = name;
        if (kind == 1) request.files = {QStringLiteral("manual.bin")};
        if (kind == 2) request.destination = QStringLiteral("destination");
        if (kind == 3) request.expectedSize = 0;
        if (kind == 4) request.protocol = SerialTransferProtocol::Ymodem;
        if (kind == 5) { request.direction = FT::Direction::Receive; request.destination = QStringLiteral("destination"); }
        if (kind == 6) request.source->open = {};
        SourceChannel channel; SerialFileTransferController controller(channel.channel());
        QVERIFY(!controller.start(request));
        QVERIFY(!controller.isActive()); QVERIFY(!channel.reserved);
        QCOMPARE(trace->opened.load(), 0); QCOMPARE(channel.activations, 0); QVERIFY(channel.output.isEmpty());
    }
    void sourceUsesWorkerAndRereadsRequestedOffsets() {
        auto trace = std::make_shared<SourceTrace>();
        trace->contents.resize(2048);
        for (qsizetype i = 0; i < trace->contents.size(); ++i) trace->contents[i] = char(i % 251);
        SourceChannel channel; SerialFileTransferController controller(channel.channel());
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QVERIFY(controller.start(sourceRequest(trace)));
        QTRY_VERIFY(!controller.progress().preparing); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ReceiveInit, 0x21002000U); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ResumePosition);
        QTRY_VERIFY(sourcePayload(channel.output).has_value());
        QCOMPARE(*sourcePayload(channel.output), trace->contents.first(1024)); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ResumePosition, 512);
        QTRY_VERIFY(sourcePayload(channel.output).has_value());
        QCOMPARE(*sourcePayload(channel.output), trace->contents.mid(512, 1024)); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::Ack, 1536);
        QTRY_VERIFY(sourcePayload(channel.output).has_value());
        QCOMPARE(*sourcePayload(channel.output), trace->contents.mid(1536, 512)); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::Ack, 2048); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ReceiveInit, 0x21002000U);
        QTRY_VERIFY(!channel.output.isEmpty()); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::Finish);
        QTRY_COMPARE(done.size(), 1); QVERIFY(done.at(0).at(0).toBool());
        QCOMPARE(channel.output, QByteArray("OO"));
        QCOMPARE(controller.progress().completedFiles, quint64(1));
        QCOMPARE(controller.progress().transferredBytes, quint64(2048));
        QVERIFY(controller.progress().totalBytes.has_value()); QCOMPARE(*controller.progress().totalBytes, quint64(2048));
        QTRY_COMPARE(trace->closed.load(), 1);
        QCOMPARE(trace->opened.load(), 1); QCOMPARE(trace->reads.load(), 3); QVERIFY(!trace->wrongThread.load());
        QVERIFY(!channel.reserved); QCOMPARE(channel.releases, 1);
        const std::vector<std::pair<quint64, qsizetype>> expected{{0, 1024}, {512, 1024}, {1536, 512}};
        std::lock_guard<std::mutex> lock(trace->mutex); QVERIFY(trace->ranges == expected);
    }
    void sourceEmptyFileCompletesWithoutRead() {
        auto trace = std::make_shared<SourceTrace>();
        SourceChannel channel; SerialFileTransferController controller(channel.channel());
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QVERIFY(controller.start(sourceRequest(trace)));
        QTRY_VERIFY(!controller.progress().preparing); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ReceiveInit, 0x21002000U); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ResumePosition); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ReceiveInit, 0x21002000U);
        QTRY_VERIFY(!channel.output.isEmpty()); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::Finish);
        QTRY_COMPARE(done.size(), 1); QVERIFY(done.at(0).at(0).toBool());
        QCOMPARE(controller.progress().completedFiles, quint64(1));
        QVERIFY(controller.progress().totalBytes.has_value()); QCOMPARE(*controller.progress().totalBytes, quint64(0));
        QTRY_COMPARE(trace->closed.load(), 1); QCOMPARE(trace->reads.load(), 0); QVERIFY(!trace->wrongThread.load());
    }
    void sourcePreparationFailureWritesNothing_data() {
        QTest::addColumn<bool>("oversize");
        QTest::newRow("open-failure") << false; QTest::newRow("size-limit") << true;
    }
    void sourcePreparationFailureWritesNothing() {
        QFETCH(bool, oversize);
        auto trace = std::make_shared<SourceTrace>();
        trace->failOpen = !oversize;
        if (oversize) trace->reportedSize = FT::MaxFileSize + 1;
        SourceChannel channel; SerialFileTransferController controller(channel.channel());
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QVERIFY(controller.start(sourceRequest(trace)));
        QTRY_COMPARE(done.size(), 1); QVERIFY(!done.at(0).at(0).toBool());
        QCOMPARE(channel.activations, 0); QVERIFY(channel.output.isEmpty()); QVERIFY(!channel.reserved);
        QCOMPARE(trace->opened.load(), 1); QCOMPARE(trace->reads.load(), 0);
        if (oversize) QTRY_COMPARE(trace->closed.load(), 1);
        QVERIFY(!trace->wrongThread.load());
    }
    void sourceReadFailureCannotBecomePayload_data() {
        QTest::addColumn<bool>("shortRead");
        QTest::newRow("failure") << false; QTest::newRow("short-read") << true;
    }
    void sourceReadFailureCannotBecomePayload() {
        QFETCH(bool, shortRead);
        auto trace = std::make_shared<SourceTrace>(); trace->contents = QByteArray(1024, 'x');
        trace->failRead = !shortRead; trace->shortRead = shortRead;
        SourceChannel channel; SerialFileTransferController controller(channel.channel());
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        QVERIFY(controller.start(sourceRequest(trace)));
        QTRY_VERIFY(!controller.progress().preparing); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ReceiveInit, 0x21002000U); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ResumePosition);
        QTRY_COMPARE(done.size(), 1); QVERIFY(!done.at(0).at(0).toBool());
        QCOMPARE(channel.output, QByteArray(5, char(0x18))); QCOMPARE(disconnected.size(), 1);
        QCOMPARE(controller.progress().completedFiles, quint64(0)); QVERIFY(!channel.reserved);
        QTRY_COMPARE(trace->closed.load(), 1); QCOMPARE(trace->reads.load(), 1); QVERIFY(!trace->wrongThread.load());
    }
    void sourceLateOpenCannotAffectAnotherTransfer_data() {
        QTest::addColumn<int>("stopKind");
        QTest::newRow("cancel") << 0; QTest::newRow("abort") << 1; QTest::newRow("destroy") << 2;
    }
    void sourceLateOpenCannotAffectAnotherTransfer() {
        QFETCH(int, stopKind);
        auto oldSource = std::make_shared<SourceTrace>(); oldSource->contents = "old"; oldSource->blockOpen = true;
        SourceGateRelease oldRelease{oldSource};
        SourceChannel channel;
        auto controller = std::make_unique<SerialFileTransferController>(channel.channel());
        QSignalSpy done(controller.get(), &SerialFileTransferController::finished);
        QVERIFY(controller->start(sourceRequest(oldSource)));
        QTRY_COMPARE(oldSource->entered.available(), 1);
        if (stopKind == 0) { controller->cancel(); QTRY_COMPARE(done.size(), 1); }
        else if (stopKind == 1) { controller->abort("connection changed"); QCOMPARE(done.size(), 1); }
        else { controller.reset(); QCOMPARE(done.size(), 0); }
        QVERIFY(!channel.reserved); channel.output.clear();
        if (!controller) controller = std::make_unique<SerialFileTransferController>(channel.channel());
        auto nextSource = std::make_shared<SourceTrace>(); nextSource->contents = "new"; nextSource->blockOpen = true;
        SourceGateRelease nextRelease{nextSource};
        QSignalSpy nextDone(controller.get(), &SerialFileTransferController::finished);
        QVERIFY(controller->start(sourceRequest(nextSource)));
        QTRY_COMPARE(nextSource->entered.available(), 1);
        const auto generation = controller->progress().transferId;
        const int activations = channel.activations;
        oldSource->resume.release(); QTRY_COMPARE(oldSource->closed.load(), 1);
        QCoreApplication::processEvents();
        QVERIFY(controller->isActive()); QVERIFY(controller->progress().preparing);
        QCOMPARE(controller->progress().transferId, generation);
        QVERIFY(!controller->progress().totalBytes.has_value());
        QCOMPARE(channel.activations, activations); QVERIFY(channel.output.isEmpty()); QCOMPARE(nextDone.size(), 0);
        controller->abort("test finished"); QCOMPARE(nextDone.size(), 1);
        nextSource->resume.release(); QTRY_COMPARE(nextSource->closed.load(), 1);
        QVERIFY(!oldSource->wrongThread.load()); QVERIFY(!nextSource->wrongThread.load());
        QVERIFY(!channel.reserved);
    }
    void sourceLateReadAfterAbortDoesNotWriteToNewTransfer() {
        auto oldSource = std::make_shared<SourceTrace>(); oldSource->contents = QByteArray(1024, 'x'); oldSource->blockRead = true;
        SourceGateRelease oldRelease{oldSource};
        SourceChannel channel; SerialFileTransferController controller(channel.channel());
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QVERIFY(controller.start(sourceRequest(oldSource)));
        QTRY_VERIFY(!controller.progress().preparing); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ReceiveInit, 0x21002000U); channel.output.clear();
        sourcePeerHeader(controller, FT::ZFrame::ResumePosition);
        QTRY_COMPARE(oldSource->entered.available(), 1);
        controller.abort("connection changed"); QCOMPARE(done.size(), 1); QVERIFY(channel.output.isEmpty());
        auto nextSource = std::make_shared<SourceTrace>(); nextSource->contents = "new"; nextSource->blockOpen = true;
        SourceGateRelease nextRelease{nextSource};
        QVERIFY(controller.start(sourceRequest(nextSource)));
        QTRY_COMPARE(nextSource->entered.available(), 1);
        oldSource->resume.release(); QTRY_COMPARE(oldSource->closed.load(), 1);
        QCoreApplication::processEvents();
        QVERIFY(controller.isActive()); QVERIFY(controller.progress().preparing);
        QVERIFY(channel.output.isEmpty()); QCOMPARE(done.size(), 1);
        QCOMPARE(oldSource->reads.load(), 1); QVERIFY(!oldSource->wrongThread.load());
        controller.abort("test finished"); nextSource->resume.release(); QTRY_COMPARE(nextSource->closed.load(), 1);
    }
    void drainedCancelKeepsConnectionWhenLateInputReachesDeadline() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath("source"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("x"), qint64(1));
        file.close();
        NovaTerm::SerialIoState state;
        const auto generation = state.requestOpen();
        QVERIFY(state.opened(generation));
        int activated = 0, clears = 0;
        bool reserved = false;
        QByteArray sent;
        QPointer<SerialFileTransferController> controller;
        const auto drain = [&] {
            for (;;) {
                const auto block = state.takeWrite(generation, 1024);
                if (block.bytes.isEmpty()) break;
                sent.append(block.bytes);
                state.completeWrite(generation, block.epoch, block.bytes.size());
            }
        };
        SerialFileTransferController::Channel channel;
        channel.connected = [&] { return state.snapshot().connected; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; };
        channel.activate = [&] { ++activated; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [&] { return state.pendingWriteBytes(); };
        channel.write = [&](QByteArrayView bytes) {
            const auto accepted = state.queueWrite(bytes);
            drain();
            return accepted;
        };
        channel.clearWrites = [&] {
            ++clears;
            if (!state.requestClear()) return false;
            const auto epoch = state.snapshot().writeEpoch;
            // 采用真实交接状态，确认另排一次事件，保留异步屏障哨兵。
            QTimer::singleShot(0, controller.data(), [&, epoch] {
                state.completeClear(generation, epoch);
                drain();
                controller->notifyWritable();
            });
            return true;
        };
        SerialFileTransferController transfer(std::move(channel));
        controller = &transfer;
        QSignalSpy done(&transfer, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&transfer, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request;
        request.protocol = SerialTransferProtocol::Zmodem;
        request.files = {file.fileName()};
        QVERIFY(transfer.start(request));
        QTRY_COMPARE(activated, 1);
        sent.clear();
        transfer.cancel();
        QTRY_VERIFY(sent.count(char(0x18)) >= 5);
        QCOMPARE(state.pendingWriteBytes(), qint64(0));
        // CAN 已排空，但晚到输入持续刷新静默窗口，直到总取消期限。
        QTimer delayed;
        delayed.setInterval(30);
        connect(&delayed, &QTimer::timeout, &transfer, [&] {
            transfer.acceptBytes(QByteArray(64, char(0)));
        });
        delayed.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2500);
        delayed.stop();
        QCOMPARE(disconnected.size(), 0);
        QCOMPARE(clears, 1);
        QVERIFY(!reserved);
        QVERIFY(state.snapshot().connected);
    }
    void expiredAsynchronousClearDisconnectsBeforeReleasingLease() {
        QTemporaryDir directory;
        QFile file(directory.filePath("source"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x"); file.close();
        bool reserved = false, clearing = false, disconnectedBeforeRelease = false;
        int activated = 0;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; };
        channel.activate = [&] { ++activated; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [&]() -> qint64 { return clearing ? 1 : 0; };
        channel.clearWrites = [&] { clearing = true; return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 { return clearing ? 0 : bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        connect(&controller, &SerialFileTransferController::disconnectRequired,
                &controller, [&] { disconnectedBeforeRelease = reserved; });
        SerialTransferRequest request;
        request.protocol = SerialTransferProtocol::Zmodem;
        request.files = {file.fileName()};
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1);
        controller.cancel();
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 2500);
        QCOMPARE(disconnected.count(), 1);
        QVERIFY(disconnectedBeforeRelease);
        QVERIFY(!reserved);
    }
    void asynchronousClearBarrierGetsItsOwnDrainBudget() {
        QTemporaryDir directory;
        QFile file(directory.filePath("source"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x"); file.close();
        qint64 pending = 0;
        bool clearing = false, reserved = false;
        int activated = 0;
        QByteArray sent;
        SerialFileTransferController::Channel channel;
        channel.baudRate = 300;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; };
        channel.activate = [&] { ++activated; };
        channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [&] { return pending; };
        channel.clearWrites = [&] { clearing = true; return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 {
            if (clearing) return 0;
            sent.append(bytes.data(), bytes.size());
            return bytes.size();
        };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request;
        request.protocol = SerialTransferProtocol::Zmodem;
        request.files = {file.fileName()};
        QVERIFY(controller.start(request));
        QTRY_COMPARE(activated, 1);
        sent.clear(); pending = 60;
        controller.cancel();
        QVERIFY(clearing); QVERIFY(reserved);
        // 模拟 300 baud 下已提交底层的有限前缀异步排空；GUI 不能提前
        // 宣称清写完成，也不能按只有 CAN 的期限丢掉取消序列。
        QTimer::singleShot(2400, &controller, [&] {
            pending = 0; clearing = false; controller.notifyWritable();
        });
        QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 4500);
        QVERIFY(sent.count(char(0x18)) >= 5);
        QCOMPARE(disconnected.count(), 0);
        QVERIFY(!reserved);
    }
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
    void negotiationCancellationKeepsConnection_data() {
        QTest::addColumn<int>("protocol"); QTest::addColumn<bool>("sending"); QTest::addColumn<bool>("remote");
        for (int protocol = 0; protocol != 5; ++protocol)
            for (const bool sending : {false, true}) for (const bool remote : {false, true})
                QTest::newRow(qPrintable(QString::number(protocol) + (sending ? "-send" : "-receive") + (remote ? "-peer-cancel" : "-user-cancel"))) << protocol << sending << remote;
    }
    void negotiationCancellationKeepsConnection() {
        QFETCH(int, protocol); QFETCH(bool, sending); QFETCH(bool, remote);
        QTemporaryDir directory; QFile source(directory.filePath("source"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("payload"); source.close();
        QByteArray transport, cancellationWire; int activated = 0; bool reserved = false, recording = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.activate = [&] { ++activated; }; channel.release = [&] { reserved = false; };
        channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [&] { return transport.size(); };
        channel.clearWrites = [&] { transport.clear(); return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 {
            const auto count = std::min<qsizetype>(2, bytes.size());
            transport.append(bytes.first(count));
            if (recording) cancellationWire.append(bytes.first(count));
            return count;
        };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        QSignalSpy visible(&controller, &SerialFileTransferController::visibleRemainder);
        std::unique_ptr<FT::ITransferEngine> peer;
        if (protocol == 3) peer = std::make_unique<FT::YmodemEngine>();
        else if (protocol == 4) peer = std::make_unique<FT::ZmodemEngine>();
        else peer = std::make_unique<FT::XmodemEngine>();
        FT::TransferRequest other; other.direction = sending ? FT::Direction::Receive : FT::Direction::Send;
        other.files = {{"peer.bin", 7}}; other.expectedSize = 7;
        if (protocol == 0) other.config.xmodemMode = FT::XmodemMode::Checksum;
        if (protocol == 2) other.config.xmodemMode = FT::XmodemMode::OneK;
        QVERIFY(peer->start(other, 0));
        if (auto action = peer->takeAction()) QVERIFY(peer->completeOperation(action->id, {}, 0));
        const auto initial = peer->pendingOutput();
        QVERIFY(peer->acknowledgeOutput(std::size_t(initial.size), 0));
        SerialTransferRequest request; request.protocol = SerialTransferProtocol(protocol);
        request.direction = sending ? FT::Direction::Send : FT::Direction::Receive;
        if (sending) request.files = {source.fileName()};
        else { request.destination = protocol <= 2 ? directory.filePath("received.bin") : directory.path(); request.expectedSize = 7; }
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1); recording = true;
        if (remote) {
            peer->cancel(1); const auto can = peer->pendingOutput();
            const QByteArray bytes(can.data, can.size); QVERIFY(peer->acknowledgeOutput(std::size_t(can.size), 1));
            controller.acceptBytes(bytes);
        } else controller.cancel();
        QTimer drain; drain.setInterval(2);
        QObject::connect(&drain, &QTimer::timeout, &controller, [&] {
            if (transport.isEmpty()) { controller.notifyWritable(); return; }
            const QByteArray bytes = transport.first(1); transport.remove(0, 1);
            peer->consume({bytes.constData(), bytes.size()}, 2); controller.notifyWritable();
            const auto response = peer->pendingOutput();
            if (!response.empty()) {
                const QByteArray reply(response.data, response.size);
                peer->acknowledgeOutput(std::size_t(response.size), 2); controller.acceptBytes(reply);
            }
        });
        drain.start(); QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2500); drain.stop();
        QCOMPARE(peer->progress().state, FT::State::Cancelled); QCOMPARE(cancellationWire, QByteArray(5, char(0x18)));
        QCOMPARE(disconnected.size(), 0); QVERIFY(!controller.isActive()); QVERIFY(!reserved); QCOMPARE(visible.size(), 0);
        const auto beforeLate = cancellationWire; controller.notifyWritable(); controller.acceptBytes("post-cancel text");
        QCOMPARE(cancellationWire, beforeLate); QCOMPARE(visible.size(), 0); // 非活动数据由 Session 正常入站处理。
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 2); controller.abort("test cleanup");
        QCOMPARE(disconnected.size(), 0);
    }
    void cancellationFinishesStartedXyPacket_data() {
        QTest::addColumn<int>("protocol"); QTest::addColumn<bool>("peerCancelled");
        for (int protocol = 0; protocol < 4; ++protocol) {
            QTest::newRow(qPrintable(QString::number(protocol) + "-user-cancel")) << protocol << false;
            QTest::newRow(qPrintable(QString::number(protocol) + "-peer-cancel")) << protocol << true;
        }
    }
    void cancellationFinishesStartedXyPacket() {
        QFETCH(int, protocol); QFETCH(bool, peerCancelled);
        QTemporaryDir directory;
        QFile source(directory.filePath("source.bin"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write(QByteArray(1300,'x')); source.close();
        QByteArray transport;
        bool reserved = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; };
        channel.activate = [] {}; channel.coreIdle = [] { return true; };
        channel.pendingWriteBytes = [&] { return transport.size(); };
        channel.clearWrites = [&] { transport.clear(); return true; };
        channel.write = [&](QByteArrayView bytes) { transport.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol(protocol); request.files = {source.fileName()};
        QVERIFY(controller.start(request)); QTRY_VERIFY(!controller.progress().preparing);
        std::unique_ptr<FT::ITransferEngine> peer;
        if (protocol == 3) peer = std::make_unique<FT::YmodemEngine>();
        else peer = std::make_unique<FT::XmodemEngine>();
        FT::TransferRequest peerRequest;
        peerRequest.config.xmodemMode = protocol == 0 ? FT::XmodemMode::Checksum
            : protocol == 2 ? FT::XmodemMode::OneK : FT::XmodemMode::Crc;
        QVERIFY(peer->start(peerRequest, 0));
        while (const auto action = peer->takeAction()) QVERIFY(peer->completeOperation(action->id, {}, 0));
        const auto init = peer->pendingOutput();
        const QByteArray handshake(init.data, init.size);
        QVERIFY(peer->acknowledgeOutput(std::size_t(init.size),0)); controller.acceptBytes(handshake);
        QTRY_VERIFY(transport.size() > 17);
        // 对端已读取包前缀，CAN 此时不能被当作剩余 payload；必须保留当前包尾。
        const auto prefix = transport.first(17);
        QCOMPARE(peer->consume({prefix.constData(), prefix.size()},0).consumed,std::size_t(17));
        transport.remove(0,17); controller.notifyWritable();
        if (peerCancelled) {
            peer->cancel(1);
            const auto abort = peer->pendingOutput();
            const QByteArray sequence(abort.data,abort.size);
            QVERIFY(peer->acknowledgeOutput(std::size_t(abort.size),1));
            controller.acceptBytes(sequence);
            // 对端已退出时，不能继续补文件包到它的命令行。
            QCOMPARE(transport,QByteArray(5,char(0x18)));
        } else controller.cancel();
        int turns = 0;
        while (!transport.isEmpty() && !FT::terminal(peer->progress().state) && ++turns < 16) {
            const auto result = peer->consume({transport.constData(), transport.size()},0);
            transport.remove(0,qsizetype(result.consumed));
            while (const auto action = peer->takeAction()) QVERIFY(peer->completeOperation(action->id, {}, 0));
        }
        QCOMPARE(peer->progress().state,FT::State::Cancelled);
        transport.clear(); controller.notifyWritable();
        QTRY_VERIFY(!controller.isActive());
        QCOMPARE(disconnected.size(),0); QVERIFY(!reserved);
    }

    void cancellationResponseAndQuietGrace_data() {
        QTest::addColumn<bool>("sending"); QTest::addColumn<int>("reply");
        QTest::newRow("verified-peer-can-then-prompt") << true << 0;
        QTest::newRow("unverified-prompt-dropped") << true << 1;
        QTest::newRow("binary-after-can-dropped") << true << 2;
        QTest::newRow("oversize-prompt-dropped") << true << 3;
        QTest::newRow("receiving-payload-remains-private") << false << 0;
    }
    void cancellationResponseAndQuietGrace() {
        QFETCH(bool, sending); QFETCH(int, reply); QTemporaryDir directory;
        QFile source(directory.filePath("source")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        int activated = 0; QByteArray sent, visible;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; }; channel.release = [] {};
        channel.activate = [&] { ++activated; }; channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [] { return 0; };
        channel.clearWrites = [] { return true; }; channel.write = [&](QByteArrayView bytes) { sent.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        QObject::connect(&controller, &SerialFileTransferController::visibleRemainder, &controller, [&](const QByteArray& bytes) { visible += bytes; });
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc;
        request.direction = sending ? FT::Direction::Send : FT::Direction::Receive;
        if (sending) request.files = {source.fileName()}; else request.destination = directory.filePath("received.bin");
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1); controller.cancel(); QVERIFY(controller.isActive());
        QTest::qWait(80);
        QByteArray response = reply == 1 ? QByteArray() : QByteArray(5, char(0x18));
        response += reply == 2 ? QByteArray("\0binary", 7) : reply == 3 ? QByteArray(8193, 'p') : QByteArray("peer> ");
        controller.acceptBytes(response); QTest::qWait(90); QVERIFY(controller.isActive());
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1500); QCOMPARE(disconnected.size(), 0);
        QCOMPARE(visible, sending && reply == 0 ? QByteArray("peer> ") : QByteArray());
    }
    void peerCancelKeepsVerifiedPrompt_data() {
        QTest::addColumn<int>("protocol"); QTest::addColumn<int>("delivery");
        QTest::addColumn<bool>("sending");
        for (int protocol = 0; protocol < 5; ++protocol)
            for (int delivery = 0; delivery < 6; ++delivery)
                for (const bool sending : {true, false})
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(protocol).arg(delivery)
                        .arg(sending ? QStringLiteral("send") : QStringLiteral("receive"))))
                        << protocol << delivery << sending;
    }
    void peerCancelKeepsVerifiedPrompt() {
        QFETCH(int, protocol); QFETCH(int, delivery); QFETCH(bool, sending);
        QTemporaryDir directory; QFile source(directory.filePath("source"));
        QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write("payload"), qint64(7)); source.close();
        bool reserved = false; int activated = 0; QByteArray output, visible;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.coreIdle = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; }; channel.activate = [&] { ++activated; };
        channel.pendingWriteBytes = [] { return 0; }; channel.clearWrites = [] { return true; };
        channel.write = [&](QByteArrayView bytes) { output.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        QObject::connect(&controller, &SerialFileTransferController::visibleRemainder, &controller,
                         [&](const QByteArray& bytes) { visible.append(bytes); });
        SerialTransferRequest request; request.protocol = SerialTransferProtocol(protocol);
        request.direction = sending ? FT::Direction::Send : FT::Direction::Receive;
        if (sending) request.files = {source.fileName()};
        else request.destination = protocol < 3 ? directory.filePath("received") : directory.path();
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1);
        const QByteArray can(protocol == 4 ? 5 : 2, char(0x18));
        const QByteArray prompt("\r\npeer> ");
        if (delivery == 0) controller.acceptBytes(can + prompt);
        else if (delivery == 1) {
            controller.acceptBytes(can.first(1));
            controller.acceptBytes(can.sliced(1) + QByteArray(3, char(0x18)) + QByteArray(2, '\b') + "\r\npe");
            controller.acceptBytes("er> ");
        } else {
            controller.acceptBytes(can);
            if (delivery == 2) controller.acceptBytes(prompt);
            else if (delivery == 3) controller.acceptBytes(QByteArray("\0binary", 7) + prompt);
            else if (delivery == 4) controller.acceptBytes(QByteArray(8193, 'p'));
            else controller.acceptBytes(prompt + QByteArray("\xFF", 1));
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 1500);
        QVERIFY(!done.front().front().toBool()); QVERIFY(!controller.isActive()); QVERIFY(!reserved);
        QCOMPARE(disconnected.size(), 0); QCOMPARE(output.right(5), QByteArray(5, char(0x18)));
        QCOMPARE(visible, sending && delivery < 3 ? prompt : QByteArray());
    }
    void cancellationDeadlineCrossedDuringChannelQueryStillFinishes() {
        QTemporaryDir directory; QFile source(directory.filePath("source"));
        QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write("x"), qint64(1)); source.close();
        bool reserved = false, crossDeadline = false, delayedQuery = false; int activated = 0;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.coreIdle = [] { return true; };
        channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; }; channel.activate = [&] { ++activated; };
        channel.clearWrites = [] { return true; }; channel.write = [](QByteArrayView bytes) { return bytes.size(); };
        channel.pendingWriteBytes = [&] {
            if (crossDeadline) { crossDeadline = false; delayedQuery = true; QThread::msleep(100); }
            return qint64(0);
        };
        SerialFileTransferController controller(channel);
        QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc;
        request.files = {source.fileName()}; QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1);
        // 晚到字节保持静默窗口未结束；在总取消期限前模拟一次线程调度延迟，
        // 使 pending 查询返回时总期限已过、静默期限仍在未来。
        QTimer lateInput, crossing, timeout; QEventLoop loop;
        lateInput.setInterval(20); crossing.setSingleShot(true); crossing.setTimerType(Qt::PreciseTimer);
        timeout.setSingleShot(true);
        QObject::connect(&lateInput, &QTimer::timeout, &controller, [&] { controller.acceptBytes(QByteArray(1, char(0))); });
        QObject::connect(&crossing, &QTimer::timeout, &controller, [&] {
            lateInput.stop(); crossDeadline = true; controller.acceptBytes(QByteArray(1, char(0)));
        });
        QObject::connect(&controller, &SerialFileTransferController::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        controller.cancel(); lateInput.start(); crossing.start(1430); timeout.start(2600); loop.exec();
        lateInput.stop(); crossing.stop(); timeout.stop();
        QVERIFY(delayedQuery); QCOMPARE(done.size(), 1); QVERIFY(!controller.isActive()); QVERIFY(!reserved);
        QCOMPARE(disconnected.size(), 0);
    }
    void stalledOrMalformedCancelRemainsBounded_data() {
        QTest::addColumn<int>("mode");
        QTest::newRow("write-zero") << 0;
        QTest::newRow("accepted-but-stalled") << 1;
        QTest::newRow("invalid-write-count") << 2;
        QTest::newRow("transport-write-error") << 3;
        QTest::newRow("continuous-delayed-input") << 4;
    }
    void stalledOrMalformedCancelRemainsBounded() {
        QFETCH(int, mode); QTemporaryDir directory;
        QFile file(directory.filePath("source")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("x"); file.close();
        qint64 pending = 0; int activated = 0, clears = 0; bool reserved = false;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [&](quint64) { reserved = true; return true; };
        channel.release = [&] { reserved = false; }; channel.activate = [&] { ++activated; };
        channel.coreIdle = [] { return true; }; channel.pendingWriteBytes = [&] { return pending; };
        channel.clearWrites = [&] { ++clears; pending = 0; return true; };
        channel.write = [&](QByteArrayView bytes) -> qint64 {
            if (mode == 0) return 0;
            if (mode == 1) { pending += bytes.size(); return bytes.size(); }
            if (mode == 2) return bytes.size() + 1;
            if (mode == 3) return -1;
            return bytes.size();
        };
        SerialFileTransferController controller(channel); QSignalSpy done(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        QSignalSpy visible(&controller, &SerialFileTransferController::visibleRemainder);
        SerialTransferRequest request; request.protocol = SerialTransferProtocol::XmodemCrc; request.files = {file.fileName()};
        QVERIFY(controller.start(request)); QTRY_COMPARE(activated, 1); QElapsedTimer elapsed; elapsed.start(); controller.cancel();
        QTimer delayed; delayed.setInterval(30);
        QObject::connect(&delayed, &QTimer::timeout, &controller, [&] { controller.acceptBytes(QByteArray(64 * 1024, char(0))); controller.notifyWritable(); });
        if (mode == 4) delayed.start();
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 2500); delayed.stop();
        QVERIFY(elapsed.elapsed() < 2300); QCOMPARE(disconnected.size(), 0); QCOMPARE(visible.size(), 0); QVERIFY(!reserved);
        QCOMPARE(pending, qint64(0));
        // 已排空的 X/Y 取消只等静默期限，不需要再清写空队列。
        if (mode == 4) QCOMPARE(clears, 0);
        else QVERIFY(clears >= 1);
        controller.notifyWritable(); QCOMPARE(done.size(), 1);
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
        QVERIFY(visible.isEmpty()); QCOMPARE(disconnected.size(), 0);
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
    void actualDrainGatesAckAndCancelKeepsConnection() {
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
        controller.cancel(); QVERIFY(controller.isActive()); QCOMPARE(transport, QByteArray(1, char(4)) + QByteArray(5, char(0x18)));
        transport.clear(); controller.notifyWritable(); QVERIFY(controller.isActive());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.isActive(), 1000); QCOMPARE(disconnected.size(), 0);
        // 迟到写入通知不能改变结束状态或向旧串口写入。
        controller.notifyWritable(); QCOMPARE(disconnected.size(), 0); QVERIFY(transport.isEmpty());
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
    void cancelReadResumeAllowsSynchronousPeerReply() {
        QTemporaryDir directory; QFile source(directory.filePath("source"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        bool ready = false; QByteArray output, visible;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; };
        channel.release = [] {}; channel.activate = [] {};
        channel.coreIdle = [&] { return ready; }; channel.pendingWriteBytes = [] { return 0; };
        channel.clearWrites = [] { return true; };
        channel.write = [&](QByteArrayView bytes) { output.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel);
        QSignalSpy disconnected(&controller,&SerialFileTransferController::disconnectRequired);
        QObject::connect(&controller,&SerialFileTransferController::visibleRemainder,&controller,
            [&](const QByteArray& bytes) { visible += bytes; });
        QObject::connect(&controller,&SerialFileTransferController::readPauseChanged,&controller,
            [&](bool paused) {
                if (!paused && controller.isActive()) {
                    ready = true;
                    controller.acceptBytes(QByteArray(5,char(0x18)) + QByteArray("peer> "));
                }
            });
        SerialTransferRequest request; request.files = {source.fileName()};
        QVERIFY(controller.start(request)); controller.acceptBytes(QByteArray(192*1024,'C'));
        controller.cancel(); QTRY_VERIFY(!controller.isActive());
        QCOMPARE(disconnected.size(),0); QCOMPARE(output,QByteArray(5,char(0x18)));
        QCOMPARE(visible,QByteArray("peer> "));
    }

    void preparingCancelHonorsCoreBarrierBeforeSendingCan() {
        QTemporaryDir directory; QFile source(directory.filePath("source")); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("data"); source.close();
        bool idle = false, queued = false; qint64 pending = 7; int activations = 0, clears = 0;
        QByteArray sent; SerialFileTransferController* pointer = nullptr;
        SerialFileTransferController::Channel channel;
        channel.connected = [] { return true; }; channel.reserve = [](quint64) { return true; }; channel.release = [] {};
        channel.activate = [&] { ++activations; };
        channel.coreIdle = [&] {
            if (idle && !queued) { queued = true; QMetaObject::invokeMethod(pointer, [&] { pending = 9; }, Qt::QueuedConnection); }
            return idle;
        };
        channel.pendingWriteBytes = [&] { return pending; };
        channel.clearWrites = [&] { ++clears; pending = 0; return true; };
        channel.write = [&](QByteArrayView bytes) { sent.append(bytes); return bytes.size(); };
        SerialFileTransferController controller(channel); pointer = &controller;
        QSignalSpy finished(&controller, &SerialFileTransferController::finished);
        QSignalSpy disconnected(&controller, &SerialFileTransferController::disconnectRequired);
        SerialTransferRequest request; request.files = {source.fileName()};
        QVERIFY(controller.start(request)); controller.cancel(); QVERIFY(controller.isActive());
        QCOMPARE(clears, 0); QCOMPARE(activations, 0); QVERIFY(sent.isEmpty());
        idle = true; controller.notifyWritable(); QTRY_COMPARE(pending, qint64(9));
        QCOMPARE(clears, 0); QCOMPARE(activations, 0); QVERIFY(sent.isEmpty());
        pending = 0; controller.notifyWritable(); QTRY_COMPARE(activations, 1);
        QTRY_COMPARE(finished.size(), 1); QCOMPARE(sent, QByteArray(5, char(0x18)));
        QCOMPARE(disconnected.size(), 0); QVERIFY(!controller.isActive());
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
        QVERIFY(controller.start(request)); controller.cancel();
        QVERIFY(controller.isActive()); QTRY_VERIFY_WITH_TIMEOUT(!controller.isActive(), 2500);
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
