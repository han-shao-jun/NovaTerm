#include "TerminalSession.h"

#include "SessionInputPump.h"
#include "core/terminal/TerminalCore.h"

TerminalSession::TerminalSession(TerminalCore* core, QObject* parent)
    : QObject(parent)
    , _core(core)
{
}

TerminalSession::~TerminalSession()
{
    detach();
}

void TerminalSession::attach(ITransport* transport, Ownership ownership)
{
    if (_transport == transport)
        return;
    detach();
    if (!transport || !_core)
        return;

    _transport = transport;
    _ownership = ownership;
    _disconnectObserved = std::make_shared<bool>(false);
    _attachmentActive = std::make_shared<bool>(true);
    if (ownership == Ownership::Adopt)
        transport->setParent(this);

    _inputPump = new SessionInputPump(transport, _core, this);
    _inputPump->start();
    _coreOutputConnection = QObject::connect(
        _core, &TerminalCore::outputData, this,
        [this, transport](const QByteArray& data) {
            if (_transport == transport && transport->isConnected())
                transport->write(data);
        });

    _transportConnections.append(QObject::connect(
        transport, &ITransport::connected, this, [this, transport] {
        if (_transport == transport)
            emit connected(transport);
    }));
    _transportConnections.append(QObject::connect(
        transport, &ITransport::errorOccurred, this,
        [this, transport](const QString& error) {
        if (_transport == transport)
            emit errorOccurred(transport, error);
    }));
    _transportConnections.append(QObject::connect(
        transport, &ITransport::exited, this,
        [this, transport](quint32 exitCode, TransportExitReason reason) {
        if (_transport == transport)
            emit exited(transport, exitCode, reason);
    }));

    const auto disconnectedObserved = _disconnectObserved;
    const auto attachmentActive = _attachmentActive;
    const auto handleDisconnected = [this, transport, ownership,
                                     disconnectedObserved, attachmentActive] {
        if (*disconnectedObserved)
            return;
        *disconnectedObserved = true;
        const bool wasCurrent = *attachmentActive;
        *attachmentActive = false;
        if (wasCurrent) {
            stopPump();
            QObject::disconnect(_coreOutputConnection);
            _coreOutputConnection = {};
            _transport = nullptr;
        }
        if (ownership == Ownership::Adopt)
            transport->deleteLater();
        if (wasCurrent)
            emit disconnected(transport);
    };
    const auto disconnectedConnection = QObject::connect(
        transport, &ITransport::disconnected, this, handleDisconnected);
    if (ownership == Ownership::Borrowed)
        _transportConnections.append(disconnectedConnection);

    _transportConnections.append(QObject::connect(
        transport, &QObject::destroyed, this,
        [this, disconnectedObserved, attachmentActive] {
        const bool wasCurrent = *attachmentActive;
        *attachmentActive = false;
        if (wasCurrent) {
            stopPump();
            QObject::disconnect(_coreOutputConnection);
            _coreOutputConnection = {};
            _transport = nullptr;
        }
        if (!*disconnectedObserved && wasCurrent) {
            *disconnectedObserved = true;
            emit disconnected(nullptr);
        }
    }));
}

void TerminalSession::detach()
{
    ITransport* transport = _transport.data();
    stopPump();
    QObject::disconnect(_coreOutputConnection);
    _coreOutputConnection = {};
    for (const QMetaObject::Connection& connection : std::as_const(_transportConnections))
        QObject::disconnect(connection);
    _transportConnections.clear();
    if (!transport)
        return;
    const Ownership ownership = _ownership;
    const auto disconnectedObserved = _disconnectObserved;
    const auto attachmentActive = _attachmentActive;
    *attachmentActive = false;
    _transport = nullptr;
    transport->setReadPaused(false);
    transport->disconnect();
    if (ownership == Ownership::Adopt && !*disconnectedObserved
        && !transport->hasPendingDisconnect()) {
        *disconnectedObserved = true;
        transport->deleteLater();
        emit disconnected(transport);
    }
}

bool TerminalSession::start()
{
    return _transport && _transport->connectToHost();
}

void TerminalSession::write(const QByteArray& data)
{
    if (_transport && _transport->isConnected())
        _transport->write(data);
}

void TerminalSession::resize(int columns, int rows)
{
    if (_transport)
        _transport->resizeTerminal(columns, rows);
}

void TerminalSession::stopPump()
{
    if (!_inputPump)
        return;
    delete _inputPump;
    _inputPump = nullptr;
}
