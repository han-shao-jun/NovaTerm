/**
 * @file InteractiveSessionCommandExecutor.cpp
 * @brief 交互 Coordinator 的通用 Executor 适配实现。
 */
#include "InteractiveSessionCommandExecutor.h"

#include "SessionCommandCoordinator.h"

#include <utility>

InteractiveSessionCommandExecutor::InteractiveSessionCommandExecutor(
    SessionCommandCoordinator* coordinator, CommandPlatformProfile profile,
    QString targetFingerprint, QObject* parent)
    : ISessionCommandExecutor(parent)
    , _coordinator(coordinator)
    , _profile(std::move(profile))
    , _targetFingerprint(std::move(targetFingerprint))
{
    if (_coordinator) {
        connect(_coordinator, &SessionCommandCoordinator::finished,
                this, &ISessionCommandExecutor::finished);
    }
}

bool InteractiveSessionCommandExecutor::isAvailable() const
{
    return _coordinator && _profile.isAvailable()
        && !_targetFingerprint.isEmpty();
}

CommandExecutorCapabilities
InteractiveSessionCommandExecutor::capabilities() const
{
    // Profile 尚未声明退出码与终止证据前保持保守；Task 4 再按可信
    // Shell/设备 Profile 提升对应能力。
    return {CommandExecutionMode::InteractiveFramed, false, false, false};
}

CommandPlatformProfile InteractiveSessionCommandExecutor::profile() const
{
    return _profile;
}

QString InteractiveSessionCommandExecutor::targetFingerprint() const
{
    return _targetFingerprint;
}

bool InteractiveSessionCommandExecutor::execute(
    const CommandExecutionRequest& request)
{
    return isAvailable() && _coordinator->submit(request);
}

void InteractiveSessionCommandExecutor::cancel(quint64 requestId)
{
    if (_coordinator)
        _coordinator->cancel(requestId);
}
