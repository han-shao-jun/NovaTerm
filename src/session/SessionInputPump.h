#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>

class ITransport;
class TerminalCore;

// Owns the bounded Transport -> TerminalCore byte path. The parser remains the
// sole writer; the GUI only submits bytes and observes overload diagnostics.
class SessionInputPump final : public QObject
{
    Q_OBJECT
public:
    SessionInputPump(ITransport* transport, TerminalCore* core,
                     QObject* parent = nullptr);
    ~SessionInputPump() override;

    void start();
    void stop();

private:
    void acceptBytes(const QByteArray& data);
    void handleBackpressure(bool paused);
    void drainPending();
    void reportOverload(const QString& reason);

    static constexpr qsizetype MaxPendingBytes = 8 * 1024 * 1024;

    QPointer<ITransport> _transport;
    QPointer<TerminalCore> _core;
    QByteArray _pending;
    bool _running{false};
};
