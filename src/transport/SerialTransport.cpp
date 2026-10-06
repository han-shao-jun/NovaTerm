/**
 * @file   SerialTransport.cpp
 * @brief  串口字节传输实现。
 *
 * 基于 QSerialPort 的 readyRead/errorOccurred 信号驱动 I/O，写入直接
 * 转发给 QSerialPort 并由其 bytesWritten 信号反馈进度。连接打开通过
 * QueuedConnection 延迟到事件循环，避免在会话创建栈上重入发信号。
 */
#include "SerialTransport.h"

#include <QMetaEnum>
#include <QMetaObject>
#include <QSignalBlocker>
#include <utility>
#include <algorithm>

namespace {

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

} // namespace

SerialTransport::SerialTransport(SerialConfig config, QObject* parent)
    : ITransport(parent)
    , _config(std::move(config))
{
    // 读缓冲按串口物理速率定容：921600 baud ≈ 92 KB/s，256 KiB 足够
    // 吸收暂停消费时的 ~3 秒突发。原 8 MiB 要 ~12 分钟不消费才攒满，
    // 上限与业务完全不成比例（缓冲只在暂停 drain 时才真正占用）。
    _port.setReadBufferSize(256 * 1024);

    connect(&_port, &QSerialPort::readyRead,
            this, &SerialTransport::readAvailable);
    connect(&_port, &QSerialPort::errorOccurred,
            this, &SerialTransport::handleError);
    connect(&_port, &QSerialPort::bytesWritten,
            this, &SerialTransport::bytesWritten);
}

SerialTransport::~SerialTransport()
{
    disconnect();
}

bool SerialTransport::connectToHost()
{
    if (_port.isOpen())
        disconnect();

    if (!_config.isValid()) {
        reportError(tr("Invalid serial port configuration."),
                    TransportErrorCategory::Configuration);
        return false;
    }

    _errorString.clear();
    _readPaused = false;
    _connectPending = true;
    _disconnectEmitted = false;
    _port.setPortName(_config.portName.trimmed());
    const QString settingDetail = applyPortSettings(_port, _config);
    if (!settingDetail.isEmpty()) {
        _connectPending = false;
        reportError(tr("Cannot apply serial settings on %1 (%2)")
                        .arg(_config.portName, settingDetail),
                    TransportErrorCategory::Configuration);
        return false;
    }

    // 将实际打开推迟到事件循环，避免调用方在会话创建栈上重入地
    // 收到 connected()/errorOccurred() 信号。
    QMetaObject::invokeMethod(this, [this]() {
        if (!_connectPending || _port.isOpen())
            return;
        _connectPending = false;
        if (!_port.open(QIODevice::ReadWrite)) {
            reportError(tr("Cannot open serial port %1: %2")
                            .arg(_config.portName, _port.errorString()),
                        TransportErrorCategory::Connection, true);
            return;
        }
        // 打开后再下发一次：上面的调用只写进了 Qt 的缓存，这份才真正
        // 走到驱动。失败时必须关掉端口并上报配置错误，否则 isConnected()
        // 会为真而线路参数并不匹配。QSignalBlocker 压掉 Qt 顺带发出的
        // errorOccurred，统一由下面这一条 Configuration 错误出口。
        QString appliedDetail;
        {
            const QSignalBlocker blocker(_port);
            appliedDetail = applyPortSettings(_port, _config);
        }
        if (!appliedDetail.isEmpty()) {
            _port.close();
            reportError(tr("Serial port %1 rejects %2: %3")
                            .arg(_config.portName, appliedDetail,
                                 _port.errorString()),
                        TransportErrorCategory::Configuration);
            return;
        }
        emit connected();
    }, Qt::QueuedConnection);
    return true;
}

void SerialTransport::disconnect()
{
    const bool wasOpen = _port.isOpen();
    const bool wasConnecting = _connectPending;
    _connectPending = false;
    _readPaused = false;
    if (wasOpen)
        _port.close();

    if ((wasOpen || wasConnecting) && !_disconnectEmitted) {
        _disconnectEmitted = true;
        emit disconnected();
    }
}

void SerialTransport::write(const QByteArray& data)
{
    if (data.isEmpty())
        return;
    if (!_port.isOpen()) {
        reportError(tr("Serial port is not connected."),
                    TransportErrorCategory::Io);
        return;
    }
    if (_port.bytesToWrite() + data.size() > MaxPendingWriteBytes) {
        reportError(tr("Serial write queue exceeded its 1 MiB limit."),
                    TransportErrorCategory::Overload);
        return;
    }

    const qint64 accepted = _port.write(data);
    if (accepted < 0)
        reportError(tr("Serial write failed: %1").arg(_port.errorString()),
                    TransportErrorCategory::Io, true);
}

void SerialTransport::resizeTerminal(int cols, int rows)
{
    Q_UNUSED(cols);
    Q_UNUSED(rows);
    // 串口链路无远程 PTY/窗口尺寸概念，空实现。
}

bool SerialTransport::isConnected() const
{
    return _port.isOpen();
}

QString SerialTransport::errorString() const
{
    return _errorString;
}

bool SerialTransport::setReadPaused(bool paused)
{
    _readPaused = paused;
    if (!paused)
        readAvailable();
    return true;
}

void SerialTransport::readAvailable()
{
    if (_readPaused || !_port.isOpen())
        return;

    while (_port.bytesAvailable() > 0 && !_readPaused) {
        const QByteArray data = _port.read(64 * 1024);
        if (data.isEmpty())
            break;
        emit readyRead(data);
    }
}

void SerialTransport::handleError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || error == QSerialPort::NotOpenError)
        return;

    const bool connectionLost = error == QSerialPort::ResourceError
        || error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError;
    reportError(tr("Serial port %1: %2")
                    .arg(_config.portName, _port.errorString()),
                error == QSerialPort::PermissionError
                    ? TransportErrorCategory::Permission
                    : (connectionLost ? TransportErrorCategory::Connection
                                      : TransportErrorCategory::Io),
                connectionLost);
    if (connectionLost)
        disconnect();
}

void SerialTransport::reportError(const QString& message,
                                  TransportErrorCategory category,
                                  bool retryable)
{
    _errorString = message;
    emit transportError(TransportError{category, 0, message, retryable});
    emit errorOccurred(_errorString);
}

qint64 SerialTransport::tryWriteBounded(QByteArrayView data, qint64 limit)
{
    if (!_port.isOpen()) return -1;
    if (data.isEmpty()) return 0;
    const qint64 available = std::min(limit, MaxPendingWriteBytes) - _port.bytesToWrite();
    if (available <= 0) return 0;
    const qint64 size = std::min<qint64>(available, data.size());
    const qint64 accepted = _port.write(data.data(), size);
    if (accepted < 0)
        reportError(tr("Serial write failed: %1").arg(_port.errorString()),
                    TransportErrorCategory::Io, true);
    return accepted;
}
qint64 SerialTransport::pendingWriteBytes() const
{
    return _port.isOpen() ? _port.bytesToWrite() : 0;
}
bool SerialTransport::clearPendingOutput()
{
    return _port.isOpen() && _port.clear(QSerialPort::Output);
}
