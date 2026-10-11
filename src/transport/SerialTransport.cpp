/**
 * @file   SerialTransport.cpp
 * @brief  串口字节传输实现。
 *
 * 独立 I/O 事件循环拥有 QSerialPort，有界收发队列与合并通知连接 GUI
 * 门面。底层 bytesWritten 反馈实际进度，清写屏障保护取消后的字节顺序。
 */
#include "SerialTransport.h"
#include "SerialDriverReceiveQueue.h"
#include "core/ThreadNaming.h"
#include <QThread>
#include <QTimer>
#include <atomic>

#include <QMetaEnum>
#include <QMetaObject>
#include <QSignalBlocker>
#include <QPointer>
#include <utility>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
/** @brief 原生队列操作仅用于打开后的初始化，不改写传输字节。 */
class WindowsSerialQueueDriver final
{
public:
    explicit WindowsSerialQueueDriver(QSerialPort& port)
        : _handle(reinterpret_cast<HANDLE>(port.handle()))
    {
    }

    bool request(std::uint32_t receiveBytes, std::uint32_t transmitBytes,
                 std::uint32_t& error) const
    {
        const bool accepted = SetupComm(_handle, static_cast<DWORD>(receiveBytes),
                                       static_cast<DWORD>(transmitBytes)) != FALSE;
        error = accepted ? 0 : static_cast<std::uint32_t>(GetLastError());
        return accepted;
    }

    bool query(std::uint32_t& actualBytes, std::uint32_t& error) const
    {
        COMMPROP properties{};
        properties.wPacketLength = static_cast<WORD>(sizeof(properties));
        properties.dwProvSpec1 = COMMPROP_INITIALIZED;
        const bool queried = GetCommProperties(_handle, &properties) != FALSE;
        error = queried ? 0 : static_cast<std::uint32_t>(GetLastError());
        actualBytes = queried
            ? static_cast<std::uint32_t>(properties.dwCurrentRxQueue) : 0;
        return queried;
    }

private:
    HANDLE _handle; ///< QSerialPort 拥有，只在当前打开调用内借用
};
#endif

/**
 * @brief 下发串口线路参数并逐项校验 setter 返回值。
 *
 * @param port 已设置端口名的 QSerialPort，可处于关闭或打开状态。
 * @param config 待下发的线路参数。
 * @return 空字符串表示五项参数全部生效；否则返回点名首个被拒参数及其
 *         请求值的描述，供调用方拼进用户可见消息。
 * @note 端口关闭时 QSerialPort 的 setter 只缓存取值（恒返回 true），
 *       真正触达驱动的校验发生在 open() 内部的 SetCommState/tcsetattr，
 *       因此打开之后必须再下发一次才能拿到真实结果。
 */
QString applyPortSettings(QSerialPort& port, const SerialConfig& config)
{
    // 逐项下发而不是用 && 串联：短路会跳过后面的判定，出错时无法指明是
    // 哪一项被驱动拒绝。五项一律视为致命：波特率或帧格式与请求不符时
    // 链路只剩乱码，会话显示"已连接"却跑在驱动兜底速率上比直接失败更
    // 难排查——这正是丢弃返回值时无法诊断的那个故障。
    const bool baudRate = port.setBaudRate(config.baudRate);
    const bool dataBits = port.setDataBits(config.dataBits);
    const bool parity = port.setParity(config.parity);
    const bool stopBits = port.setStopBits(config.stopBits);
    const bool flowControl = port.setFlowControl(config.flowControl);
    if (baudRate && dataBits && parity && stopBits && flowControl)
        return {};

    // 失败详情点名参数与请求值：波特率是数字，其余四项借用 QSerialPort
    // 自己的枚举名（Data7 / OddParity / OneStop / SoftwareControl），便于
    // 用户回到设置页逐项核对。
    if (!baudRate)
        return QStringLiteral("baud rate %1").arg(config.baudRate);
    if (!dataBits)
        return QStringLiteral("data bits %1").arg(QString::fromLatin1(
            QMetaEnum::fromType<QSerialPort::DataBits>()
                .valueToKey(static_cast<int>(config.dataBits))));
    if (!parity)
        return QStringLiteral("parity %1").arg(QString::fromLatin1(
            QMetaEnum::fromType<QSerialPort::Parity>()
                .valueToKey(static_cast<int>(config.parity))));
    if (!stopBits)
        return QStringLiteral("stop bits %1").arg(QString::fromLatin1(
            QMetaEnum::fromType<QSerialPort::StopBits>()
                .valueToKey(static_cast<int>(config.stopBits))));
    return QStringLiteral("flow control %1").arg(QString::fromLatin1(
        QMetaEnum::fromType<QSerialPort::FlowControl>()
            .valueToKey(static_cast<int>(config.flowControl))));
}


constexpr char SerialIoThreadName[] = "nvterm-serial";
std::atomic<int> ActiveSerialIoWorkers{0};

/** @brief 唯一端口线程，入口首先设置可观测 OS 名称。 */
class SerialIoThread final : public QThread
{
public:
    explicit SerialIoThread(std::shared_ptr<NovaTerm::SerialIoState> state)
        : _state(std::move(state)) {}
protected:
    void run() override
    {
        NovaTerm::setCurrentThreadName(SerialIoThreadName);
        ++ActiveSerialIoWorkers;
        _state->setWorkerRunning(true);
        QThread::run();
        _state->setWorkerRunning(false);
        --ActiveSerialIoWorkers;
    }
private:
    std::shared_ptr<NovaTerm::SerialIoState> _state;
};

/** @brief 仅在 I/O 事件循环访问端口；不持有门面、Session 或 UI 指针。 */
class SerialIoWorker final : public QObject
{
    Q_OBJECT
public:
    SerialIoWorker(SerialConfig config, std::shared_ptr<NovaTerm::SerialIoState> state)
        : _config(std::move(config)), _state(std::move(state)) {}
signals:
    void inputAvailable();
    void noticesAvailable();
    void finished();
public slots:
    void wake()
    {
        if (_finishing) return;
        _state->consumeWorkerWake();
        const auto desired = _state->snapshot();
        if (desired.stopping) {
            _finishing = true;
            closePort();
            emit finished();
            return;
        }
        if (_port && (_generation != desired.generation || !desired.desiredOpen))
            closePort();
        if (!desired.desiredOpen) {
            _state->closed(desired.generation);
            notify();
            return;
        }
        if (!_port && !openPort(desired.generation)) {
            notify();
            return;
        }
        if (_fatal) {
            const auto generation = _generation;
            closePort();
            _state->closed(generation);
            notify();
            return;
        }
        drainRead();
        drainWrite();
        notify();
    }
private:
    void notify()
    {
        if (_state->armNoticeNotification()) emit noticesAvailable();
        if (_state->armReceiveNotification()) emit inputAvailable();
    }
    void scheduleWake()
    {
        if (_state->armWorkerWake())
            QMetaObject::invokeMethod(this, &SerialIoWorker::wake, Qt::QueuedConnection);
    }
    void retry()
    {
        if (!_retry) {
            _retry = std::make_unique<QTimer>();
            _retry->setSingleShot(true);
            connect(_retry.get(), &QTimer::timeout, this, &SerialIoWorker::wake);
        }
        if (!_retry->isActive()) _retry->start(5);
    }
    void closePort()
    {
        _closing = true;
        if (_retry) _retry->stop();
        if (_port) {
            { const QSignalBlocker blocker(*_port); _port->close(); }
            _port.reset();
        }
        _generation = _writeEpoch = 0;
        _fatal = _receiveFullReported = false;
        _closing = false;
    }
    bool openPort(quint64 generation)
    {
        _generation = generation;
        _port = std::make_unique<QSerialPort>();
        _port->setReadBufferSize(256 * 1024);
        _port->setPortName(_config.portName.trimmed());
        QString settingDetail;
        bool opened = false;
        {
            const QSignalBlocker blocker(*_port);
            settingDetail = applyPortSettings(*_port, _config);
            if (settingDetail.isEmpty()) opened = _port->open(QIODevice::ReadWrite);
            if (opened) settingDetail = applyPortSettings(*_port, _config);
        }
        if (!opened || !settingDetail.isEmpty()) {
            const auto message = !settingDetail.isEmpty()
                ? SerialTransport::tr("Serial port %1 rejects %2: %3")
                    .arg(_config.portName, settingDetail, _port->errorString())
                : SerialTransport::tr("Cannot open serial port %1: %2")
                    .arg(_config.portName, _port->errorString());
            _state->reportError(generation, TransportError{
                !settingDetail.isEmpty() ? TransportErrorCategory::Configuration
                    : TransportErrorCategory::Connection,
                int(_port->error()), message, settingDetail.isEmpty()});
            closePort();
            _state->closed(generation);
            return false;
        }
        configureDriver(generation);
        _writeEpoch = _state->snapshot().writeEpoch;
        if (!_state->opened(generation)) {
            closePort();
            scheduleWake();
            return false;
        }
        connect(_port.get(), &QSerialPort::readyRead, this, [this] {
            drainRead(); notify();
        });
        connect(_port.get(), &QSerialPort::bytesWritten, this, [this](qint64 bytes) {
            _state->completeWrite(_generation, _writeEpoch, bytes);
            notify();
            // Qt 的写完成发射栈尚未结束，不在此栈内再清空或提交新写。
            scheduleWake();
        });
        connect(_port.get(), &QSerialPort::errorOccurred, this,
                [this](QSerialPort::SerialPortError error) {
            if (_closing || error == QSerialPort::NoError
                || error == QSerialPort::NotOpenError) return;
            const bool lost = error == QSerialPort::ResourceError
                || error == QSerialPort::DeviceNotFoundError
                || error == QSerialPort::PermissionError;
            _state->reportError(_generation, TransportError{
                error == QSerialPort::PermissionError ? TransportErrorCategory::Permission
                    : (lost ? TransportErrorCategory::Connection : TransportErrorCategory::Io),
                int(error), SerialTransport::tr("Serial port %1: %2")
                    .arg(_config.portName, _port->errorString()), lost});
            _fatal = _fatal || lost;
            notify();
            // 不能在 QSerialPort 自己的信号栈中析构端口。
            if (lost) scheduleWake();
        });
        return true;
    }
    void configureDriver(quint64 generation)
    {
#ifdef Q_OS_WIN
        WindowsSerialQueueDriver driver(*_port);
        auto result = _state->snapshot();
        std::uint32_t beforeError = 0;
        result.driverCapacityQueriedBefore = driver.query(result.driverRxBefore, beforeError);
        const auto queue = NovaTerm::initializeSerialDriverReceiveQueue(driver, result.driverRxRequested);
        result.driverRequestAccepted = queue.requestAccepted;
        result.driverCapacityQueried = queue.capacityQueried;
        result.driverRxActual = queue.actualBytes;
        result.driverSetupError = queue.requestError;
        result.driverQueryError = queue.queryError;
        _state->setDriverQueue(generation, result);
        if (!queue.verified()) {
            _state->reportWarning(generation, SerialTransport::tr(
                "Serial port %1 receive buffer could not be verified "
                "(requested %2 bytes, reported %3 bytes, setup error %4, query error %5). "
                "Continuing with the driver's buffer; burst input may be lost.")
                .arg(_config.portName).arg(result.driverRxRequested).arg(result.driverRxActual)
                .arg(result.driverSetupError).arg(result.driverQueryError));
        }
#else
        static_cast<void>(generation);
#endif
    }
    void drainRead()
    {
        if (!_port || !_port->isOpen() || _fatal) return;
        // Qt 自己的缓冲也有上限；收件箱满后停止排空，已有字节不丢弃。
        while (_port->bytesAvailable() > 0) {
            const auto room = _state->receiveRoom(_generation);
            if (room <= 0) {
                if (!_receiveFullReported) {
                    _receiveFullReported = true;
                    _state->reportWarning(_generation, SerialTransport::tr(
                        "Serial receive queue reached its 8 MiB limit. "
                        "Input delivery must resume; flow control is required to prevent overrun."));
                }
                break;
            }
            _receiveFullReported = false;
            auto bytes = _port->read(std::min(room, NovaTerm::SerialIoState::ChunkBytes));
            if (bytes.isEmpty()) break;
            if (!_state->receive(_generation, std::move(bytes))) break;
        }
    }
    void drainWrite()
    {
        if (!_port || !_port->isOpen() || _fatal) return;
        const auto current = _state->snapshot();
        if (current.generation != _generation || !current.connected) return;
        if (current.clearPending) {
            // 仅保留已经提交底层的一个小前缀。Windows Qt 6.8 的 PurgeComm
            // 会中止在途 WriteFile 并产生 ResourceError，不能用它伪造清写
            // 完成；新数据在此有限前缀排空和屏障确认之后才允许提交。
            if (_port->bytesToWrite() != 0) { retry(); return; }
            if (_state->completeClear(_generation, current.writeEpoch))
                _writeEpoch = current.writeEpoch;
        }
        if (_port->bytesToWrite() != 0) return;
        // 底层只保留一个 1 KiB 前缀，取消不会继续排出整段 GUI 出件箱。
        const auto block = _state->takeWrite(_generation, 1024);
        if (block.bytes.isEmpty()) return;
        _writeEpoch = block.epoch;
        const auto accepted = _port->write(block.bytes);
        if (accepted < 0) {
            _state->reportError(_generation, TransportError{TransportErrorCategory::Io,
                int(_port->error()), SerialTransport::tr("Serial write failed: %1")
                    .arg(_port->errorString()), true});
            _fatal = true;
            scheduleWake();
            return;
        }
        _state->returnUnwritten(block, accepted);
        if (accepted == 0) retry();
    }
    const SerialConfig _config;
    std::shared_ptr<NovaTerm::SerialIoState> _state;
    std::unique_ptr<QSerialPort> _port;
    std::unique_ptr<QTimer> _retry;
    quint64 _generation{0}, _writeEpoch{0};
    bool _fatal{false}, _closing{false}, _finishing{false}, _receiveFullReported{false};
};

} // namespace

class SerialTransport::Impl
{
public:
    std::shared_ptr<NovaTerm::SerialIoState> state = std::make_shared<NovaTerm::SerialIoState>();
    bool started{false};
    void start(SerialTransport* owner)
    {
        started = true;
        auto* thread = new SerialIoThread(state);
        auto* worker = new SerialIoWorker(owner->_config, state);
        thread->setObjectName(QString::fromLatin1(SerialIoThreadName));
        worker->moveToThread(thread);
        QObject::connect(owner, &SerialTransport::ioWakeRequested,
                         worker, &SerialIoWorker::wake, Qt::QueuedConnection);
        QObject::connect(thread, &QThread::started,
                         worker, &SerialIoWorker::wake, Qt::QueuedConnection);
        QObject::connect(worker, &SerialIoWorker::inputAvailable,
                         owner, &SerialTransport::deliverInput, Qt::QueuedConnection);
        QObject::connect(worker, &SerialIoWorker::noticesAvailable,
                         owner, &SerialTransport::deliverNotices, Qt::QueuedConnection);
        // quit 排到 I/O 事件循环，避免在 worker 的信号发射栈内收束生命周期。
        QObject::connect(worker, &SerialIoWorker::finished, worker,
                         [thread] { thread->quit(); }, Qt::QueuedConnection);
        QObject::connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }
};

SerialTransport::SerialTransport(SerialConfig config, QObject* parent)
    : ITransport(parent), _config(std::move(config)), _impl(std::make_unique<Impl>())
{
    _state = _impl->state;
}

SerialTransport::~SerialTransport()
{
    // 状态与 worker 共享，门面销毁后端口仍能在 I/O 线程安全关闭。
    _impl->state->requestStop();
    if (_impl->started) wakeIo();
}

bool SerialTransport::connectToHost()
{
    if (!_config.isValid()) {
        reportError(tr("Invalid serial port configuration."), TransportErrorCategory::Configuration);
        return false;
    }
    _errorString.clear();
    _connected = false;
    _connectPending = true;
    _disconnectEmitted = false;
    _impl->state->requestOpen();
    if (!_impl->started) _impl->start(this);
    else wakeIo();
    return true;
}

void SerialTransport::disconnect()
{
    if (!_connected && !_connectPending && !_impl->state->snapshot().closePending) return;
    _connected = false;
    _connectPending = false;
    _impl->state->requestClose();
    if (_impl->started) wakeIo();
}

void SerialTransport::wakeIo()
{
    if (_impl->state->armWorkerWake()) emit ioWakeRequested();
}

void SerialTransport::deliverInput()
{
    const QPointer<SerialTransport> guard(this);
    const auto state = _impl->state;
    const auto generation = state->snapshot().generation;
    if (!_connected) { state->finishReceiveNotification(false); return; }
    for (int chunk = 0; chunk < 4; ++chunk) {
        auto bytes = state->takeReceived();
        if (bytes.isEmpty()) break;
        emit readyRead(bytes);
        if (!guard) return;
        if (state->snapshot().generation != generation) {
            if (state->finishReceiveNotification(guard->_connected))
                QMetaObject::invokeMethod(guard, &SerialTransport::deliverInput, Qt::QueuedConnection);
            return;
        }
    }
    wakeIo();
    if (state->finishReceiveNotification())
        QMetaObject::invokeMethod(this, &SerialTransport::deliverInput, Qt::QueuedConnection);
}

void SerialTransport::deliverNotices()
{
    const auto state = _impl->state;
    const auto notices = state->takeNotices();
    const QPointer<SerialTransport> guard(this);
    const auto current = [&] {
        return guard && state->snapshot().generation == notices.generation;
    };
    if (!current()) return;
    if (notices.opened && state->snapshot().connected) {
        _connected = true;
        _connectPending = false;
        emit connected();
        if (!current()) return;
        if (state->armReceiveNotification())
            QMetaObject::invokeMethod(this, &SerialTransport::deliverInput, Qt::QueuedConnection);
    }
    if (notices.error) {
        _connectPending = false;
        const auto error = *notices.error;
        _errorString = error.message;
        emit transportError(error);
        if (!current()) return;
        emit errorOccurred(error.message);
        if (!current()) return;
    }
    if (!notices.warning.isEmpty() && _connected) {
        emit warningOccurred(notices.warning);
        if (!current()) return;
    }
    if (_connected && (notices.written > 0 || notices.writable)
        && state->snapshot().writeEpoch == notices.writeEpoch) {
        emit bytesWritten(notices.written);
        if (!current()) return;
    }
    if (notices.closed) {
        _connected = _connectPending = false;
        if (!_disconnectEmitted) {
            _disconnectEmitted = true;
            emit disconnected();
        }
    }
}

qint64 SerialTransport::tryWriteBounded(QByteArrayView data, qint64 limit)
{
    const auto accepted = _impl->state->queueWrite(data, limit);
    if (accepted > 0) wakeIo();
    return accepted;
}

void SerialTransport::write(const QByteArray& data)
{
    if (data.isEmpty()) return;
    if (!isConnected()) {
        reportError(tr("Serial port is not connected."), TransportErrorCategory::Io);
        return;
    }
    const auto snapshot = _impl->state->snapshot();
    if (data.size() > MaxPendingWriteBytes - snapshot.queuedWriteBytes - snapshot.inFlightWriteBytes) {
        reportError(tr("Serial write queue exceeded its 1 MiB limit."), TransportErrorCategory::Overload);
        return;
    }
    if (tryWriteBounded(data, MaxPendingWriteBytes) != data.size())
        reportError(tr("Serial write failed."), TransportErrorCategory::Io, true);
}

void SerialTransport::resizeTerminal(int cols, int rows) { Q_UNUSED(cols); Q_UNUSED(rows); }
bool SerialTransport::isConnected() const { return _connected && _state->snapshot().connected; }
bool SerialTransport::hasPendingDisconnect() const { return _impl->state->snapshot().closePending; }
QString SerialTransport::errorString() const { return _errorString; }
NovaTerm::SerialIoSnapshot SerialTransport::ioStatistics() const { return _impl->state->snapshot(); }
int SerialTransport::activeIoWorkerCount() noexcept { return ActiveSerialIoWorkers.load(); }

bool SerialTransport::setReadPaused(bool paused)
{
    _impl->state->setDeliveryPaused(paused);
    if (!paused && _impl->state->armReceiveNotification())
        QMetaObject::invokeMethod(this, &SerialTransport::deliverInput, Qt::QueuedConnection);
    return true;
}

qint64 SerialTransport::pendingWriteBytes() const
{
    const auto state = _state->snapshot();
    if (_connected && !state.connected) return -1;
    return state.queuedWriteBytes + state.inFlightWriteBytes
        + (state.clearPending ? 1 : 0);
}
bool SerialTransport::clearPendingOutput()
{
    if (!_impl->state->requestClear()) return false;
    wakeIo();
    return true;
}

void SerialTransport::reportError(const QString& message, TransportErrorCategory category, bool retryable)
{
    const QPointer<SerialTransport> guard(this);
    _errorString = message;
    emit transportError(TransportError{category, 0, message, retryable});
    if (guard) emit guard->errorOccurred(message);
}

#ifdef NOVATERM_SERIAL_STRESS
void SerialTransport::setDriverReceiveQueueForTesting(quint32 bytes)
{
    _impl->state->setDriverQueueRequest(bytes);
}
#endif

#include "SerialTransport.moc"
