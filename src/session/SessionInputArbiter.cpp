/**
 * @file SessionInputArbiter.cpp
 * @brief Session 出站输入的 Lease、世代与用户抢占实现。
 */
#include "SessionInputArbiter.h"

#include "transport/ITransport.h"

SessionInputArbiter::SessionInputArbiter(QObject* parent)
    : QObject(parent)
{
}

void SessionInputArbiter::bind(ITransport* transport, quint64 generation)
{
    _transport = transport;
    _generation = generation;
    clearLease();
}

void SessionInputArbiter::reset(quint64 generation)
{
    _generation = generation;
    clearLease();
}

bool SessionInputArbiter::acquireMcpLease(quint64 executionId,
                                          quint64 generation)
{
    if (executionId == 0 || generation != _generation || _executionId != 0
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
    if (data.isEmpty())
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
