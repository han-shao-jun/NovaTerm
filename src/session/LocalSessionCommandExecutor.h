/**
 * @file LocalSessionCommandExecutor.h
 * @brief 通过固定独立 helper 执行本机诊断命令。
 */
#pragma once

#include "ISessionCommandExecutor.h"

#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <optional>

/** @brief LocalShell 的独立子进程 Executor；不接触当前 PTY/ConPTY。 */
class LocalSessionCommandExecutor final : public ISessionCommandExecutor
{
    Q_OBJECT
public:
    explicit LocalSessionCommandExecutor(QString helperPath,
                                         QStringList fixedArguments = {},
                                         QObject* parent = nullptr);
    ~LocalSessionCommandExecutor() override;

    [[nodiscard]] bool isAvailable() const override;
    [[nodiscard]] CommandExecutorCapabilities capabilities() const override;
    [[nodiscard]] CommandPlatformProfile profile() const override;
    [[nodiscard]] QString targetFingerprint() const override;
    [[nodiscard]] bool execute(const CommandExecutionRequest& request) override;
    void cancel(quint64 requestId) override;

private:
    void drainStandardOutput();
    void drainStandardError();
    void appendOutput(QByteArray bytes, QByteArray& destination);
    void requestStop(CommandExecutionOutcome outcome);
    void complete(CommandExecutionOutcome outcome, bool terminationConfirmed,
                  std::optional<int> exitCode = std::nullopt);

    QString _helperPath;
    QStringList _fixedArguments;
    QProcess _process;
    QTimer _timeout;
    std::optional<CommandExecutionRequest> _request;
    std::optional<CommandExecutionOutcome> _forcedOutcome;
    QByteArray _standardOutput;
    QByteArray _standardError;
    bool _executionMayHaveStarted{false};
    bool _outputTruncated{false};
};
