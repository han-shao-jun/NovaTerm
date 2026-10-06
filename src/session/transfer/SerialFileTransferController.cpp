/** @file SerialFileTransferController.cpp @brief 串口独占、异步磁盘与有界协议驱动。 */
#include "SerialFileTransferController.h"
#include "TransferFileWorker.h"
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/ZmodemEngine.h"
#include "filetransfer/XyPacketCodec.h"
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <limits>
namespace FT = NovaTerm::FileTransfer;
namespace {
constexpr qsizetype InputLimit = 256 * 1024;
constexpr qsizetype InputHigh = 192 * 1024;
constexpr qsizetype InputLow = 64 * 1024;
constexpr qint64 OutputLimit = 16 * 1024;
QString errorText(FT::Error error)
{
    switch (error) {
    case FT::Error::InvalidRequest:
        return QCoreApplication::translate("SerialFileTransferController", "The transfer request is invalid. Check the protocol and selected files.");
    case FT::Error::Protocol:
        return QCoreApplication::translate("SerialFileTransferController", "Invalid data from the peer. Check that both devices use the same protocol.");
    case FT::Error::UnsupportedVariant:
        return QCoreApplication::translate("SerialFileTransferController", "The peer requested an unsupported protocol variant.");
    case FT::Error::RetryLimit:
        return QCoreApplication::translate("SerialFileTransferController", "The retry limit was reached. Check the serial connection and peer.");
    case FT::Error::Timeout:
        return QCoreApplication::translate("SerialFileTransferController", "The device did not respond in time. Check the connection and peer.");
    case FT::Error::FileIo:
        return QCoreApplication::translate("SerialFileTransferController", "File access failed. Check the source file and receiving folder.");
    case FT::Error::SizeMismatch:
        return QCoreApplication::translate("SerialFileTransferController", "The transferred file size does not match the declared size.");
    case FT::Error::ResourceLimit:
        return QCoreApplication::translate("SerialFileTransferController", "The transfer exceeded its memory or file limit.");
    default:
        return QCoreApplication::translate("SerialFileTransferController", "The transfer failed.");
    }
}
QString stateText(FT::State state)
{
    switch (state) {
    case FT::State::Negotiating: return QCoreApplication::translate("SerialFileTransferController", "Waiting for peer handshake");
    case FT::State::Transferring: return QCoreApplication::translate("SerialFileTransferController", "Transferring");
    case FT::State::Finishing: return QCoreApplication::translate("SerialFileTransferController", "Committing file");
    case FT::State::Closing: return QCoreApplication::translate("SerialFileTransferController", "Waiting for final confirmation");
    case FT::State::Completed: return QCoreApplication::translate("SerialFileTransferController", "Transfer completed");
    case FT::State::Cancelled: return QCoreApplication::translate("SerialFileTransferController", "Transfer cancelled");
    case FT::State::Failed: return QCoreApplication::translate("SerialFileTransferController", "Transfer failed");
    default: return QCoreApplication::translate("SerialFileTransferController", "Preparing transfer");
    }
}
}
class SerialFileTransferController::Impl {
public:
    Impl(SerialFileTransferController* owner, Channel channel)
        : q(owner), channel(std::move(channel))
    {
        clock.start(); timer.setSingleShot(true);
        const auto derivedTimeout = std::max<quint64>(15000,
            4ULL * quint64(OutputLimit) * 12 * 1000 / quint64(std::max(1, this->channel.baudRate)));
        writeDrainTimeout = this->channel.writeDrainTimeoutMs > 0
            ? quint64(this->channel.writeDrainTimeoutMs)
            : std::min<quint64>(derivedTimeout, std::numeric_limits<int>::max());
        QObject::connect(&timer, &QTimer::timeout, q, [this] { drive(); });
    }
    ~Impl() { timer.stop(); stopWorker(); if (active && channel.release) channel.release(); }
    FT::TimePoint now() const { return FT::TimePoint(clock.elapsed()); }
    void stopWorker() { if (worker) { worker->requestStop(); worker = nullptr; } busy = false; }
    void pauseCheck()
    {
        const auto used = input.size() + tail.size() + closingPacket.size();
        if (!paused && used >= InputHigh) { paused = true; emit q->readPauseChanged(true); }
        else if (paused && used <= InputLow) { paused = false; emit q->readPauseChanged(false); }
    }
    void update()
    {
        progress.active = active; progress.preparing = active && !started;
        progress.elapsedMs = now() - began;
        if (engine && started && !cancelling) {
            const auto p = engine->progress(); progress.transferredBytes = p.transferredBytes;
            progress.completedFiles = p.completedFiles; progress.status = stateText(p.state);
            if (p.fileIndex < prepared.files.size()) {
                const auto& file = prepared.files[p.fileIndex];
                progress.currentFile = QString::fromUtf8(file.name.data(), qsizetype(file.name.size()));
            }
        }
        emit q->progressChanged(progress);
    }
    void finish(bool success, const QString& message, bool disconnect = false)
    {
        if (!active || ending) return;
        ending = true;
        timer.stop(); stopWorker(); ++epoch;
        if (success) {
            const auto finalProgress = engine->progress();
            progress.transferredBytes = finalProgress.transferredBytes;
            progress.completedFiles = finalProgress.completedFiles;
            tail.append(closingPacket); tail.append(input);
            if (!tail.isEmpty()) emit q->visibleRemainder(tail);
        }
        input.clear(); tail.clear(); closingPacket.clear();
        const bool resumeRead = paused; paused = false;
        if (!success) error = message;
        // 先要求 Session 断开，避免解除独占后新输入进入残留二进制通道。
        if (disconnect) emit q->disconnectRequired();
        active = false; started = false; cancelling = false; submitted = 0;
        if (channel.release) channel.release();
        progress.status = message; ending = false; update(); emit q->activeChanged(false);
        if (resumeRead) emit q->readPauseChanged(false);
        emit q->finished(success, message);
    }
    void fail(const QString& message)
    {
        error = message;
        if (!started) { finish(false, message); return; }
        beginCancel(message);
    }
    void beginCancel(const QString& message)
    {
        if (cancelling) return;
        stopWorker(); cancelling = true; cancelMessage = message; cancelDeadline = now() + 1500;
        input.clear(); tail.clear(); closingPacket.clear(); pauseCheck();
        submitted = 0; cancelBytes = QByteArray(5, char(0x18));
        progress.status = message;
        if (!channel.clearWrites || !channel.clearWrites()) { finish(false, message, true); return; }
        drive();
    }
    void cancellingDrive()
    {
        if (!channel.connected || !channel.connected() || now() >= cancelDeadline) {
            finish(false, cancelMessage, true); return;
        }
        const auto pending = channel.pendingWriteBytes ? channel.pendingWriteBytes() : 0;
        if (!cancelBytes.isEmpty() && pending < OutputLimit && channel.write) {
            const auto count = channel.write(QByteArrayView(cancelBytes).first(std::min<qint64>(cancelBytes.size(), OutputLimit - pending)));
            if (count < 0 || count > cancelBytes.size()) { finish(false, cancelMessage, true); return; }
            cancelBytes.remove(0, count);
        }
        if (cancelBytes.isEmpty() && channel.pendingWriteBytes && channel.pendingWriteBytes() == 0) {
            finish(false, cancelMessage, true); return;
        }
        timer.start(int(std::min<quint64>(20, cancelDeadline - now()))); update();
    }
    void preparationDrive()
    {
        if (now() - began >= 5000) { finish(false, QCoreApplication::translate("SerialFileTransferController", "Transfer preparation timed out")); return; }
        if (!ready || !channel.coreIdle || !channel.coreIdle()) { timer.start(10); return; }
        if (!barrier) {
            barrier = true;
            const auto generation = epoch;
            // queued 屏障让已发布的旧 Core outputReady 先由 Session 处理。
            QMetaObject::invokeMethod(q, [this, generation] {
                if (active && epoch == generation) { barrierPassed = true; drive(); }
            }, Qt::QueuedConnection);
            return;
        }
        if (!barrierPassed || !channel.coreIdle() || !channel.pendingWriteBytes
            || channel.pendingWriteBytes() != 0) { timer.start(10); return; }
        if (channel.activate) channel.activate();
        started = true;
        if (!engine->start(prepared, now())) { fail(QCoreApplication::translate("SerialFileTransferController", "Invalid protocol request")); return; }
        driveAgain = true;
    }
    /** @brief 收尾窗口保留普通提示文本，并响应有效重复结束包。 */
    bool consumeClosingByte(quint8 byte)
    {
        if (closingPacket.isEmpty() && byte == 0x18) {
            if (closingCanSeen) engine->cancel(now());
            closingCanSeen = true;
            return true;
        }
        closingCanSeen = false;
        if (closingPacket.isEmpty() && byte == 4) {
            engine->consume({reinterpret_cast<const char*>(&byte), 1}, now());
            return true;
        }
        if (request.protocol <= SerialTransferProtocol::Xmodem1K) {
            if (byte == 4) engine->consume({reinterpret_cast<const char*>(&byte), 1}, now());
            else tail.append(char(byte));
            return true;
        }
        if (closingPacket.isEmpty() && byte != 1 && byte != 2) { tail.append(char(byte)); return true; }
        closingPacket.append(char(byte));
        const auto result = closingCodec.push(byte, true);
        if (result == FT::XyPacketCodec::Result::Valid || result == FT::XyPacketCodec::Result::Invalid) {
            if (result == FT::XyPacketCodec::Result::Valid && closingCodec.packet().number == 0
                && std::all_of(closingCodec.packet().bytes.begin(), closingCodec.packet().bytes.end(), [](quint8 c) { return c == 0; }))
                engine->consume({closingPacket.constData(), closingPacket.size()}, now());
            else tail.append(closingPacket);
            closingPacket.clear(); closingCodec.reset();
        }
        return true;
    }
    void protocolDrive()
    {
        // 仅用实际待写量确认已排空前缀；入站先于下一包输出提交。
        const auto pending = channel.pendingWriteBytes ? channel.pendingWriteBytes() : -1;
        if (pending < 0) { fail(QCoreApplication::translate("SerialFileTransferController", "Invalid serial write queue state")); return; }
        if (pending <= submitted) {
            const auto drained = submitted - pending;
            if (drained) {
                submitted -= drained; engine->acknowledgeOutput(std::size_t(drained), now());
                lastOutputProgress = now();
            }
        }
        int budget = 8192;
        qsizetype consumedInput = 0;
        while (consumedInput < input.size() && budget-- > 0) {
            const auto p = engine->progress();
            if (FT::terminal(p.state)) break;
            if (p.state == FT::State::Closing && request.protocol != SerialTransferProtocol::Zmodem
                && request.direction == FT::Direction::Receive) { consumeClosingByte(quint8(input.at(consumedInput++))); continue; }
            const auto result = engine->consume({input.constData() + consumedInput, 1}, now());
            if (!result.consumed) break;
            consumedInput += qsizetype(result.consumed);
        }
        input.remove(0, consumedInput);
        pauseCheck();
        engine->advance(now());
        const auto p = engine->progress();
        if (p.state == FT::State::Failed || p.state == FT::State::Cancelled) {
            const auto message = p.state == FT::State::Cancelled
                ? QCoreApplication::translate("SerialFileTransferController", "Transfer cancelled")
                : (error.isEmpty() ? errorText(p.error) : error);
            fail(message); return;
        }
        if (!busy) {
            if (auto action = engine->takeAction()) {
                progress.currentFile = QString::fromUtf8(action->file.name.data(), qsizetype(action->file.name.size()));
                if (action->kind == FT::ActionKind::OfferFile) {
                    receiveLengthKnown = receiveLengthKnown && action->file.size.has_value();
                    if (receiveLengthKnown) {
                        receiveTotal += *action->file.size;
                        progress.totalBytes = receiveTotal;
                    } else progress.totalBytes.reset();
                }
                if (!worker || !worker->submit(std::move(*action))) { fail(QCoreApplication::translate("SerialFileTransferController", "File worker is unavailable")); return; }
                busy = true;
            }
        }
        const auto output = engine->pendingOutput();
        if (output.size > submitted && pending < OutputLimit) {
            const auto count = std::min<qint64>(output.size - submitted, OutputLimit - pending);
            const auto accepted = channel.write ? channel.write(QByteArrayView(output.data + submitted, count)) : -1;
            if (accepted < 0 || accepted > count) { fail(QCoreApplication::translate("SerialFileTransferController", "Serial write failed")); return; }
            submitted += accepted;
            if (accepted) lastOutputProgress = now();
            // 同步 Fake/底层允许立即排空；下一轮仍先消费入站反馈。
            if (accepted && channel.pendingWriteBytes() < submitted) driveAgain = true;
        }
        if (p.state == FT::State::Completed && submitted == 0 && pending == 0 && engine->pendingOutput().empty()) {
            finish(true, QCoreApplication::translate("SerialFileTransferController", "Transfer completed")); return;
        }
        update();
        auto deadline = engine->nextDeadline();
        const bool hostOutputPending = submitted > 0 || pending > 0 || !engine->pendingOutput().empty();
        if (hostOutputPending) {
            if (!lastOutputProgress) lastOutputProgress = now();
            const auto drainDeadline = *lastOutputProgress + writeDrainTimeout;
            if (now() >= drainDeadline) {
                fail(QCoreApplication::translate("SerialFileTransferController", "The serial output stopped progressing. Check flow control and the connection.")); return;
            }
            // 即使驱动没有 writable 通知，也以有界间隔重试与检查真实排空。
            const auto pollDeadline = std::min<quint64>(drainDeadline, now() + 50);
            deadline = deadline ? std::min<FT::TimePoint>(*deadline, pollDeadline) : pollDeadline;
        } else lastOutputProgress.reset();
        timer.stop();
        if (deadline) {
            const auto delay = *deadline > now() ? *deadline - now() : 0;
            timer.start(int(std::min<quint64>(delay, std::numeric_limits<int>::max())));
        }
        if (budget < 0 && !input.isEmpty()) timer.start(0);
    }
    void drive()
    {
        if (!active) return;
        if (driving) { driveAgain = true; return; }
        driving = true;
        int rounds = 0;
        do {
            driveAgain = false;
            if (!channel.connected || !channel.connected()) { finish(false, QCoreApplication::translate("SerialFileTransferController", "Serial port disconnected")); break; }
            if (cancelling) cancellingDrive();
            else if (!started) preparationDrive();
            else protocolDrive();
        } while (active && driveAgain && ++rounds < 8);
        driving = false;
        if (active && driveAgain) timer.start(0);
    }
    SerialFileTransferController* q;
    Channel channel;
    QTimer timer;
    QElapsedTimer clock;
    QPointer<TransferFileWorker> worker;
    std::unique_ptr<FT::ITransferEngine> engine;
    SerialTransferRequest request;
    FT::TransferRequest prepared;
    SerialTransferProgress progress;
    QByteArray input, tail, closingPacket, cancelBytes;
    FT::XyPacketCodec closingCodec;
    QString error, cancelMessage;
    quint64 epoch{0}, began{0}, cancelDeadline{0}, receiveTotal{0}, writeDrainTimeout{15000};
    std::optional<quint64> lastOutputProgress;
    bool receiveLengthKnown{true}, closingCanSeen{false};
    qint64 submitted{0};
    bool active{false}, started{false}, ready{false}, barrier{false}, barrierPassed{false};
    bool paused{false}, busy{false}, cancelling{false}, driving{false}, driveAgain{false}, ending{false};
};
SerialFileTransferController::SerialFileTransferController(Channel channel, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, std::move(channel))) {}
SerialFileTransferController::~SerialFileTransferController() = default;
bool SerialFileTransferController::start(const SerialTransferRequest& request)
{
    auto& d = *_impl;
    if (d.active) { d.error = QCoreApplication::translate("SerialFileTransferController", "A transfer is already active"); return false; }
    d.error.clear();
    if (!d.channel.connected || !d.channel.connected()) { d.error = QCoreApplication::translate("SerialFileTransferController", "Serial port is not connected"); return false; }
    if (int(request.protocol) < int(SerialTransferProtocol::XmodemChecksum)
        || int(request.protocol) > int(SerialTransferProtocol::Zmodem)
        || (request.direction != FT::Direction::Send && request.direction != FT::Direction::Receive)) {
        d.error = QCoreApplication::translate("SerialFileTransferController", "Invalid protocol request"); return false;
    }
    const auto xmodem = request.protocol <= SerialTransferProtocol::Xmodem1K;
    if (!d.channel.eightDataBits || (xmodem && d.channel.softwareFlowControl)
        || (request.protocol == SerialTransferProtocol::Ymodem && d.channel.softwareFlowControl)) {
        d.error = QCoreApplication::translate("SerialFileTransferController", "File transfers require 8 data bits; X/YMODEM cannot use software flow control"); return false;
    }
    if (request.direction == FT::Direction::Send
        && (request.files.isEmpty() || request.files.size() > int(FT::MaxFiles) || (xmodem && request.files.size() != 1))) {
        d.error = QCoreApplication::translate("SerialFileTransferController", "Invalid source file count"); return false;
    }
    if (request.direction == FT::Direction::Receive && request.destination.isEmpty()) {
        d.error = QCoreApplication::translate("SerialFileTransferController", "Select a receive destination"); return false;
    }
    if (request.expectedSize && *request.expectedSize > FT::MaxFileSize) {
        d.error = QCoreApplication::translate("SerialFileTransferController", "Expected length exceeds the protocol limit"); return false;
    }
    const auto id = ++d.epoch;
    if (!d.channel.reserve || !d.channel.reserve(id)) { d.error = QCoreApplication::translate("SerialFileTransferController", "The channel is reserved by another input task"); return false; }
    d.request = request; d.active = true; d.started = d.ready = d.barrier = d.barrierPassed = false;
    d.began = d.now(); d.submitted = 0; d.busy = true; d.progress = {};
    d.receiveTotal = 0; d.receiveLengthKnown = true; d.closingCanSeen = false; d.lastOutputProgress.reset();
    d.progress.transferId = id; d.progress.direction = request.direction;
    d.progress.status = QCoreApplication::translate("SerialFileTransferController", "Preparing files and channel");
    d.closingCodec.reset(); d.input.clear(); d.tail.clear(); d.closingPacket.clear();
    switch (request.protocol) {
    case SerialTransferProtocol::Ymodem: d.engine = std::make_unique<FT::YmodemEngine>(); break;
    case SerialTransferProtocol::Zmodem: d.engine = std::make_unique<FT::ZmodemEngine>(); break;
    default: d.engine = std::make_unique<FT::XmodemEngine>(); break;
    }
    auto* worker = new TransferFileWorker(id, request);
    d.worker = worker;
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(worker, &TransferFileWorker::resultReady, this, [this, id](const TransferFileResult& result) {
        auto& current = *_impl;
        if (!current.active || current.epoch != id || result.epoch != id || current.cancelling) return;
        current.busy = false;
        if (result.preparation) {
            if (!result.error.isEmpty()) { current.fail(result.error); return; }
            current.prepared = result.request;
            if (current.request.direction == FT::Direction::Send) {
                quint64 total = 0;
                for (const auto& file : current.prepared.files) total += *file.size;
                current.progress.totalBytes = total;
            }
            auto& config = current.prepared.config;
            if (current.request.protocol == SerialTransferProtocol::XmodemChecksum) config.xmodemMode = FT::XmodemMode::Checksum;
            else if (current.request.protocol == SerialTransferProtocol::Xmodem1K) config.xmodemMode = FT::XmodemMode::OneK;
            // 低波特率下给 1K 数据包及物理排空留足时间。
            config.responseTimeoutMs = std::max<quint64>(10000, 2048ULL * 12 * 1000 / quint64(std::max(50, current.channel.baudRate)));
            current.ready = true;
        } else {
            if (!result.error.isEmpty()) current.error = result.error;
            current.engine->completeOperation(result.operation, result.result, current.now());
        }
        current.drive();
    }, Qt::QueuedConnection);
    emit activeChanged(true); d.update(); worker->start(); d.drive(); return true;
}
bool SerialFileTransferController::isActive() const { return _impl->active; }
QString SerialFileTransferController::errorString() const { return _impl->error; }
SerialTransferProgress SerialFileTransferController::progress() const { return _impl->progress; }
void SerialFileTransferController::acceptBytes(const QByteArray& data)
{
    auto& d = *_impl;
    if (!d.active || d.cancelling || data.isEmpty()) return;
    if (data.size() > InputLimit - d.input.size() - d.tail.size() - d.closingPacket.size()) {
        d.fail(QCoreApplication::translate("SerialFileTransferController", "Receive buffer exceeded the 256 KiB limit")); return;
    }
    d.input.append(data); d.pauseCheck(); d.drive();
}
void SerialFileTransferController::notifyWritable() { _impl->drive(); }
void SerialFileTransferController::cancel()
{
    auto& d = *_impl;
    if (!d.active) return;
    if (!d.started) d.finish(false, QCoreApplication::translate("SerialFileTransferController", "Transfer cancelled"));
    else d.beginCancel(QCoreApplication::translate("SerialFileTransferController", "Transfer cancelled"));
}
void SerialFileTransferController::abort(const QString& reason)
{
    if (_impl->active) _impl->finish(false, reason);
}
