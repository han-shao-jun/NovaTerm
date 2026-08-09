#pragma once

#include <QByteArray>
#include <QFlags>
#include <QMetaType>
#include <QObject>
#include <QString>

enum class TransportCapability : quint32
{
    None = 0,
    PauseReads = 1U << 0,
    ResizeTerminal = 1U << 1,
    KeepAlive = 1U << 2,
    Reconnect = 1U << 3,
};
Q_DECLARE_FLAGS(TransportCapabilities, TransportCapability)
Q_DECLARE_OPERATORS_FOR_FLAGS(TransportCapabilities)

enum class TransportErrorCategory
{
    Configuration,
    Resolve,
    Connection,
    Authentication,
    HostKey,
    Permission,
    Protocol,
    Io,
    Overload,
    Unknown,
};

struct TransportError
{
    TransportErrorCategory category{TransportErrorCategory::Unknown};
    int code{0};
    QString message;
    bool retryable{false};
};
Q_DECLARE_METATYPE(TransportError)

enum class TransportExitReason
{
    NormalExit,
    FailedExit,
    Crash,
    UserClosed,
    IoError,
    StartFailed,
};
Q_DECLARE_METATYPE(TransportExitReason)

// Byte-only asynchronous transport contract. Implementations must not depend
// on terminal cells, renderers, widgets, or serialized profiles.
class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject* parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    virtual bool connectToHost() = 0;
    virtual bool connectAsync() { return connectToHost(); }
    virtual void disconnect() = 0;
    virtual void write(const QByteArray& data) = 0;
    virtual void resizeTerminal(int columns, int rows) = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;
    [[nodiscard]] virtual bool hasPendingDisconnect() const { return false; }
    virtual bool setReadPaused(bool paused)
    {
        Q_UNUSED(paused);
        return false;
    }
    [[nodiscard]] virtual TransportCapabilities capabilities() const
    {
        return TransportCapability::None;
    }
    [[nodiscard]] virtual QString errorString() const = 0;

signals:
    void connected();
    void disconnected();
    void readyRead(const QByteArray& data);
    void bytesWritten(qint64 bytes);
    void errorOccurred(const QString& error);
    void transportError(const TransportError& error);
    void exited(quint32 exitCode, TransportExitReason reason);
};
