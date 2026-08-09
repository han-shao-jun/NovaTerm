#pragma once

#include "SessionTypes.h"
#include "transport/ITransport.h"

#include <QObject>
#include <QPointer>
#include <QVector>
#include <memory>

class SessionInputPump;
class TerminalCore;

// Owns one terminal runtime and transport connection. It deliberately has no
// dependency on TerminalView, QWidget, or renderer resources.
class TerminalSession final : public QObject
{
    Q_OBJECT
public:
    enum class Ownership { Borrowed, Adopt };

    explicit TerminalSession(TerminalCore* core, QObject* parent = nullptr);
    explicit TerminalSession(RuntimeConfig config, QObject* parent = nullptr);
    ~TerminalSession() override;

    [[nodiscard]] SessionId id() const noexcept { return _sessionId; }
    [[nodiscard]] SessionState state() const noexcept { return _state; }
    [[nodiscard]] const RuntimeConfig& runtimeConfig() const noexcept { return _config; }
    [[nodiscard]] const SessionStatistics& statistics() const noexcept { return _statistics; }
    [[nodiscard]] TerminalCore* core() const noexcept { return _core; }
    [[nodiscard]] ITransport* transport() const { return _transport.data(); }

    void attach(ITransport* transport, Ownership ownership = Ownership::Adopt);
    // Compatibility path for a view that owns its session facade and opens a
    // new logical connection after the previous session reached Closed.
    bool resetForReuse();
    void detach();
    [[nodiscard]] bool start();
    void close(CloseMode mode = CloseMode::Graceful);
    [[nodiscard]] bool reconnect();
    void write(const QByteArray& data);
    void writeUserInput(const QByteArray& data) { write(data); }
    void resize(int columns, int rows);
    void resizeTerminal(int columns, int rows) { resize(columns, rows); }

signals:
    void stateChanged(SessionState state);
    void sessionError(const SessionError& error);
    void titleChanged(const QString& title);
    void activityChanged();
    void connected(ITransport* transport);
    void disconnected(ITransport* transport);
    void errorOccurred(ITransport* transport, const QString& error);
    void exited(ITransport* transport, quint32 exitCode,
                TransportExitReason reason);

private:
    bool transition(SessionState next);
    void startPump();
    void stopPump();
    void clearAttachment(bool requestDisconnect);
    void reportError(SessionErrorCategory category, const QString& message,
                     bool retryable = false, int code = 0);

    SessionId _sessionId{QUuid::createUuid()};
    SessionState _state{SessionState::Created};
    RuntimeConfig _config;
    SessionStatistics _statistics;
    std::unique_ptr<TerminalCore> _ownedCore;
    TerminalCore* _core{nullptr};
    QPointer<ITransport> _transport;
    SessionInputPump* _inputPump{nullptr};
    QMetaObject::Connection _coreOutputConnection;
    QVector<QMetaObject::Connection> _transportConnections;
    Ownership _ownership{Ownership::Borrowed};
    bool _acceptsUserInput{true};
};
