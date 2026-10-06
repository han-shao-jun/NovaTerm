/**
 * @file SessionInputArbiter.cpp
 * @brief Session 出站输入的 Lease、世代与用户抢占实现。
 */
#include "SessionInputArbiter.h"

#include "transport/ITransport.h"
#include <utility>

SessionInputArbiter::SessionInputArbiter(QObject* parent)
    : QObject(parent)
{
}

void SessionInputArbiter::bind(ITransport* transport, quint64 generation)
{
    _transport = transport;
    _generation = generation;
    clearLease();
    _transferId = 0; _transferActive = false; _transferWriter = {};
}

void SessionInputArbiter::reset(quint64 generation)
{
    _generation = generation;
    clearLease();
    _transferId = 0; _transferActive = false; _transferWriter = {};
}

bool SessionInputArbiter::acquireMcpLease(quint64 executionId,
                                          quint64 generation)
{
    if (executionId == 0 || generation != _generation || _executionId != 0
        || _transferId != 0
        || !_transport || !_transport->isConnected()) {
        return false;
    }
    _executionId = executionId;
    _mcpBytesWritten = false;
    return true;
}

bool SessionInputArbiter::submitMcpInput(quint64 executionId,
                                         const QByteArray& data)
{
    if (executionId == 0 || executionId != _executionId)
        return false;
    if (data.isEmpty())
        return true;
    if (!_transport || !_transport->isConnected())
        return false;
    _transport->write(data);
    _mcpBytesWritten = true;
    return true;
}

void SessionInputArbiter::submitUserInput(const QByteArray& data)
{
    if (data.isEmpty() || _transferId != 0)
        return;
    ++_userInputGeneration;
    emit userInputStarted(_userInputGeneration);
    if (_executionId != 0) {
        const quint64 executionId = _executionId;
        const bool executionMayHaveStarted = _mcpBytesWritten;
        clearLease();
        emit mcpPreempted(executionId, executionMayHaveStarted);
    }
    if (_transport && _transport->isConnected())
        _transport->write(data);
}

void SessionInputArbiter::releaseMcpLease(quint64 executionId)
{
    if (executionId == _executionId)
        clearLease();
}

void SessionInputArbiter::clearLease() noexcept
{
    _executionId = 0;
    _mcpBytesWritten = false;
}

bool SessionInputArbiter::acquireTransferLease(
    quint64 transferId, quint64 generation,
    std::function<qint64(QByteArrayView)> writer)
{
    if (!transferId || generation != _generation || _executionId || _transferId
        || !writer || !_transport || !_transport->isConnected()) return false;
    _transferId = transferId;
    _transferActive = false;
    _transferWriter = std::move(writer);
    return true;
}
bool SessionInputArbiter::activateTransferLease(quint64 transferId)
{
    if (!transferId || transferId != _transferId) return false;
    _transferActive = true;
    return true;
}
qint64 SessionInputArbiter::submitTransferInput(quint64 transferId, QByteArrayView data)
{
    if (!transferId || transferId != _transferId || !_transferActive
        || !_transferWriter || !_transport || !_transport->isConnected()) return -1;
    return _transferWriter(data);
}
void SessionInputArbiter::releaseTransferLease(quint64 transferId)
{
    if (!transferId || transferId != _transferId) return;
    _transferId = 0; _transferActive = false; _transferWriter = {};
}
bool SessionInputArbiter::submitTerminalOutput(const QByteArray& data)
{
    if (data.isEmpty()) return true;
    if (_transferActive || !_transport || !_transport->isConnected()) return false;
    if (_transferId) _transport->write(data);
    else submitUserInput(data);
    return true;
}
