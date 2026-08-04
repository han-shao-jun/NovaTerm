#include "SessionInputPump.h"

#include "core/terminal/TerminalCore.h"
#include "transport/ITransport.h"

SessionInputPump::SessionInputPump(ITransport* transport, TerminalCore* core,
                                   QObject* parent)
    : QObject(parent)
    , _transport(transport)
    , _core(core)
{
}

SessionInputPump::~SessionInputPump()
{
    stop();
}

void SessionInputPump::start()
{
    if (_running || !_transport || !_core)
        return;
    _running = true;
    connect(_transport, &ITransport::readyRead,
            this, &SessionInputPump::acceptBytes);
    connect(_core, &TerminalCore::inputBackpressureChanged,
            this, &SessionInputPump::handleBackpressure);
}

void SessionInputPump::stop()
{
    if (!_running)
        return;
    _running = false;
    if (_transport) {
        _transport->setReadPaused(true);
        QObject::disconnect(_transport, nullptr, this, nullptr);
    }
    if (_core)
        QObject::disconnect(_core, nullptr, this, nullptr);
    _pending.clear();
}

void SessionInputPump::acceptBytes(const QByteArray& data)
{
    if (!_running || data.isEmpty())
        return;

    if (!_pending.isEmpty()) {
        const qsizetype available = MaxPendingBytes - _pending.size();
        if (data.size() > available) {
            reportOverload(QStringLiteral("session input pending limit exceeded"));
            return;
        }
        _pending.append(data);
        if (!_transport->setReadPaused(true))
            reportOverload(QStringLiteral("transport cannot pause reads"));
        return;
    }

    const auto result = _core->writeInput(data);
    if (result.fullyAccepted())
        return;

    const qsizetype suffixSize = data.size() - result.acceptedBytes;
    if (suffixSize > MaxPendingBytes) {
        reportOverload(QStringLiteral("session input pending limit exceeded"));
        return;
    }
    _pending.append(data.constData() + result.acceptedBytes, suffixSize);
    if (!_transport->setReadPaused(true))
        reportOverload(QStringLiteral("transport cannot pause reads"));
}

void SessionInputPump::handleBackpressure(bool paused)
{
    if (!_running)
        return;
    if (paused) {
        if (!_transport->setReadPaused(true))
            reportOverload(QStringLiteral("transport cannot pause reads"));
        return;
    }
    drainPending();
}

void SessionInputPump::drainPending()
{
    while (_running && !_pending.isEmpty()) {
        const auto result = _core->writeInput(_pending);
        if (result.acceptedBytes > 0)
            _pending.remove(0, result.acceptedBytes);
        if (!result.fullyAccepted()) {
            _transport->setReadPaused(true);
            return;
        }
    }
    if (_running)
        _transport->setReadPaused(false);
}

void SessionInputPump::reportOverload(const QString& reason)
{
    _transport->setReadPaused(true);
    emit _core->inputOverload(reason);
}
