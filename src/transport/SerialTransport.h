#pragma once

#include "ITransport.h"
#include "session/SessionTypes.h"

#include <QSerialPort>

// Event-driven serial byte transport. It deliberately has no terminal/model
// knowledge; framing configuration is resolved before construction.
class SerialTransport final : public ITransport
{
    Q_OBJECT
public:
    explicit SerialTransport(SerialConfig config, QObject* parent = nullptr);
    ~SerialTransport() override;

    bool connectToHost() override;
    void disconnect() override;
    void write(const QByteArray& data) override;
    void resizeTerminal(int cols, int rows) override;
    [[nodiscard]] bool isConnected() const override;
    [[nodiscard]] QString errorString() const override;
    bool setReadPaused(bool paused) override;
    [[nodiscard]] TransportCapabilities capabilities() const override
    {
        return TransportCapability::PauseReads
            | TransportCapability::Reconnect;
    }

    [[nodiscard]] const SerialConfig& config() const noexcept { return _config; }

private:
    void readAvailable();
    void handleError(QSerialPort::SerialPortError error);
    void reportError(const QString& message);

    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024;

    SerialConfig _config;
    QSerialPort _port;
    QString _errorString;
    bool _readPaused{false};
    bool _connectPending{false};
    bool _disconnectEmitted{false};
};
