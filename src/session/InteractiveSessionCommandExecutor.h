/**
 * @file InteractiveSessionCommandExecutor.h
 * @brief SessionCommandCoordinator 到通用 Executor 契约的适配器。
 */
#pragma once

#include "ISessionCommandExecutor.h"

#include <QPointer>

class SessionCommandCoordinator;

/** @brief 使用当前交互终端流执行命令的 Session Executor。 */
class InteractiveSessionCommandExecutor final : public ISessionCommandExecutor
{
    Q_OBJECT
public:
    InteractiveSessionCommandExecutor(SessionCommandCoordinator* coordinator,
                                      CommandPlatformProfile profile,
                                      QString targetFingerprint,
                                      QObject* parent = nullptr);

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] CommandExecutorCapabilities capabilities() const override;
    [[nodiscard]] CommandPlatformProfile profile() const override;
    [[nodiscard]] QString targetFingerprint() const override;
    [[nodiscard]] bool execute(const CommandExecutionRequest& request) override;
    void cancel(quint64 requestId) override;

private:
    QPointer<SessionCommandCoordinator> _coordinator;
    CommandPlatformProfile _profile;
    QString _targetFingerprint;
};

