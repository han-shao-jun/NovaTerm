/**
 * @file SessionCommandFacade.cpp
 * @brief Session 命令门面的 Executor 安装、转发与迟到结果守卫。
 */
#include "SessionCommandFacade.h"

#include "ISessionCommandExecutor.h"

SessionCommandFacade::SessionCommandFacade(QObject* parent)
    : QObject(parent)
{
}

SessionCommandFacade::~SessionCommandFacade()
{
    reset(_sessionGeneration);
}

void SessionCommandFacade::reset(quint64 sessionGeneration)
{
    ++_bindingSerial;
    _sessionGeneration = sessionGeneration;
    const auto activeRequests = _activeRequests;
    _activeRequests.clear();
    if (_executor) {
        for (const quint64 requestId : activeRequests)
            _executor->cancel(requestId);
    }
    _executor.reset();
}

void SessionCommandFacade::installExecutor(
    std::unique_ptr<ISessionCommandExecutor> executor,
    quint64 sessionGeneration)
{
    reset(sessionGeneration);
    if (!executor)
        return;

    const quint64 bindingSerial = _bindingSerial;
    auto* installed = executor.get();
    connect(installed, &ISessionCommandExecutor::finished, this,
            [this, installed, bindingSerial, sessionGeneration](
                const CommandExecutionResult& result) {
        if (_executor.get() != installed || _bindingSerial != bindingSerial
            || _sessionGeneration != sessionGeneration) {
            return;
        }
        _activeRequests.remove(result.requestId);
        emit finished(result);
    });
    _executor = std::move(executor);
}

bool SessionCommandFacade::isAvailable() const
{
    return _executor && _executor->isAvailable();
}

CommandExecutorCapabilities SessionCommandFacade::capabilities() const
{
    return _executor ? _executor->capabilities() : CommandExecutorCapabilities{};
}

CommandPlatformProfile SessionCommandFacade::profile() const
{
    return _executor ? _executor->profile() : CommandPlatformProfile{};
}

QString SessionCommandFacade::targetFingerprint() const
{
    return _executor ? _executor->targetFingerprint() : QString{};
}

bool SessionCommandFacade::execute(const CommandExecutionRequest& request)
{
    if (!isAvailable() || _activeRequests.contains(request.requestId))
        return false;
    _activeRequests.insert(request.requestId);
    if (_executor->execute(request))
        return true;
    _activeRequests.remove(request.requestId);
    return false;
}

void SessionCommandFacade::cancel(quint64 requestId)
{
    if (_executor)
        _executor->cancel(requestId);
}
