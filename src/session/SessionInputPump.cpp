/**
 * @file   SessionInputPump.cpp
 * @brief  会话输入泵实现：背压转送与过载处理。
 *
 * acceptBytes() 是核心路径：直接喂入解析器，满则缓存到 _pending 并暂停读取；
 * handleBackpressure() 在解析器恢复可写时 drain _pending。过载时上报并停读。
 */
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
    if (!_running || !_transport || !_core || data.isEmpty())
        return;

    _statistics.receivedBytes += static_cast<quint64>(data.size());
    if (!_pending.isEmpty()) {
        const qsizetype available = MaxPendingBytes - _pending.size();
        if (data.size() > available) {
            reportOverload(QStringLiteral("session input pending limit exceeded"));
            return;
        }
        _pending.append(data);
        _statistics.pendingBytes = _pending.size();
        if (!_transport->setReadPaused(true))
            reportOverload(QStringLiteral("transport cannot pause reads"));
        return;
    }

    qsizetype offset = 0;
    while (offset < data.size()) {
        const qsizetype chunkSize = qMin(InputChunkBytes, data.size() - offset);
        const auto result = _core->writeInput(
            QByteArrayView(data.constData() + offset, chunkSize));
        offset += result.acceptedBytes;
        _statistics.acceptedBytes += static_cast<quint64>(result.acceptedBytes);
        if (result.fullyAccepted())
            continue;

        const qsizetype suffixSize = data.size() - offset;
        if (suffixSize > MaxPendingBytes) {
            reportOverload(QStringLiteral("session input pending limit exceeded"));
            return;
        }
        _pending.append(data.constData() + offset, suffixSize);
        _statistics.pendingBytes = _pending.size();
        ++_statistics.pauseCount;
        if (!_transport->setReadPaused(true))
            reportOverload(QStringLiteral("transport cannot pause reads"));
        return;
    }
}

void SessionInputPump::handleBackpressure(bool paused)
{
    if (!_running || !_transport || !_core)
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
    while (_running && _transport && _core && !_pending.isEmpty()) {
        const qsizetype chunkSize = qMin(InputChunkBytes, _pending.size());
        const auto result = _core->writeInput(
            QByteArrayView(_pending.constData(), chunkSize));
        if (result.acceptedBytes > 0)
            _pending.remove(0, result.acceptedBytes);
        _statistics.acceptedBytes += static_cast<quint64>(result.acceptedBytes);
        _statistics.pendingBytes = _pending.size();
        if (!result.fullyAccepted()) {
            _transport->setReadPaused(true);
            return;
        }
    }
    if (_running && _transport)
        _transport->setReadPaused(false);
}

void SessionInputPump::reportOverload(const QString& reason)
{
    ++_statistics.overloadCount;
    if (_transport)
        _transport->setReadPaused(true);
    if (_core)
        emit _core->inputOverload(reason);
    emit overload(reason);
}

SessionInputPump::Statistics SessionInputPump::statistics() const
{
    auto result = _statistics;
    result.pendingBytes = _pending.size();
    return result;
}
