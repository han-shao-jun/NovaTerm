#pragma once

#include "transport/ITransport.h"

#include <QObject>
#include <QPointer>
#include <QVector>
#include <memory>

class SessionInputPump;
class TerminalCore;

class TerminalSession final : public QObject
{
    Q_OBJECT
public:
    enum class Ownership { Borrowed, Adopt };

    explicit TerminalSession(TerminalCore* core, QObject* parent = nullptr);
    ~TerminalSession() override;

    void attach(ITransport* transport, Ownership ownership = Ownership::Adopt);
    void detach();
    [[nodiscard]] ITransport* transport() const { return _transport.data(); }
    [[nodiscard]] bool start();
    void write(const QByteArray& data);
    void resize(int columns, int rows);

signals:
    void connected(ITransport* transport);
    void disconnected(ITransport* transport);
    void errorOccurred(ITransport* transport, const QString& error);
    void exited(ITransport* transport, quint32 exitCode,
                TransportExitReason reason);

private:
    void stopPump();

    TerminalCore* _core{nullptr};
    QPointer<ITransport> _transport;
    SessionInputPump* _inputPump{nullptr};
    QMetaObject::Connection _coreOutputConnection;
    QVector<QMetaObject::Connection> _transportConnections;
    std::shared_ptr<bool> _disconnectObserved;
    std::shared_ptr<bool> _attachmentActive;
    Ownership _ownership{Ownership::Borrowed};
};
