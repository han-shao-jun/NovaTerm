/**
 * @file SshSessionCommandExecutor.h
 * @brief 将 SSH 独立 exec channel 适配为 Session 命令 Executor。
 */
#pragma once

#include "ISessionCommandExecutor.h"

#include <QPointer>

class SshTransport;

/** @brief SSH 独立输出命令执行器。 */
class SshSessionCommandExecutor final : public ISessionCommandExecutor
{
    Q_OBJECT
public:
    explicit SshSessionCommandExecutor(SshTransport* transport,
                                       QObject* parent = nullptr);

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] CommandExecutorCapabilities capabilities() const override;
    [[nodiscard]] QString targetFingerprint() const override;
    [[nodiscard]] bool execute(const CommandExecutionRequest& request) override;
    void cancel(quint64 requestId) override;

private:
    QPointer<SshTransport> _transport;
};
