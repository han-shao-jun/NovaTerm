#include "SerialTransport.h"

#include <QMetaObject>
#include <utility>

SerialTransport::SerialTransport(SerialConfig config, QObject* parent)
    : ITransport(parent)
    , _config(std::move(config))
{
    _port.setReadBufferSize(8 * 1024 * 1024);

    connect(&_port, &QSerialPort::readyRead,
            this, &SerialTransport::readAvailable);
    connect(&_port, &QSerialPort::errorOccurred,
            this, &SerialTransport::handleError);
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
        reportError(tr("Invalid serial port configuration."));
        return false;
    }

    _errorString.clear();
    _readPaused = false;
    _connectPending = true;
    _disconnectEmitted = false;
    _port.setPortName(_config.portName.trimmed());
    _port.setBaudRate(_config.baudRate);
    _port.setDataBits(_config.dataBits);
    _port.setParity(_config.parity);
    _port.setStopBits(_config.stopBits);
    _port.setFlowControl(_config.flowControl);

    // Queue the actual open so callers never receive connected/error signals
    // re-entrantly from the session creation stack.
    QMetaObject::invokeMethod(this, [this]() {
        if (!_connectPending || _port.isOpen())
            return;
        _connectPending = false;
        if (!_port.open(QIODevice::ReadWrite)) {
            reportError(tr("Cannot open serial port %1: %2")
                            .arg(_config.portName, _port.errorString()));
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
        reportError(tr("Serial port is not connected."));
        return;
    }
    if (_port.bytesToWrite() + data.size() > MaxPendingWriteBytes) {
        reportError(tr("Serial write queue exceeded its 1 MiB limit."));
        return;
    }

    const qint64 accepted = _port.write(data);
    if (accepted < 0)
        reportError(tr("Serial write failed: %1").arg(_port.errorString()));
}

void SerialTransport::resizeTerminal(int cols, int rows)
{
    Q_UNUSED(cols);
    Q_UNUSED(rows);
    // Serial links have no remote PTY/window-size capability.
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
                    .arg(_config.portName, _port.errorString()));
    if (connectionLost)
        disconnect();
}

void SerialTransport::reportError(const QString& message)
{
    _errorString = message;
    emit errorOccurred(_errorString);
}
