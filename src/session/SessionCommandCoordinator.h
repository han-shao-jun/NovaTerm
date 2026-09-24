/**
 * @file SessionCommandCoordinator.h
 * @brief Session 交互命令的提示符、Lease、捕获与完成状态机。
 */
#pragma once

#include "CommandExecutionTypes.h"
#include "InteractiveStreamFramer.h"

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <optional>

class SessionInputArbiter;

/** @brief 在 Session 层协调一条有界交互命令事务。 */
class SessionCommandCoordinator final : public QObject
{
    Q_OBJECT
public:
    SessionCommandCoordinator(SessionInputArbiter* arbiter,
                              InteractiveStreamFramer* framer,
                              QObject* parent = nullptr);

    void reset(quint64 sessionGeneration);
    [[nodiscard]] bool submit(const CommandExecutionRequest& request);
    void cancel(quint64 requestId);
    [[nodiscard]] bool isPromptReady() const noexcept;
    [[nodiscard]] quint64 promptGeneration() const noexcept
    {
        return _promptGeneration;
    }

public slots:
    void handleInteractiveEvent(const InteractiveStreamEvent& event);
    void handleInteractiveBytes(const QByteArray& bytes);

signals:
    void finished(const CommandExecutionResult& result);

private:
    void handlePreempted(quint64 executionId, bool executionMayHaveStarted);
    void finish(CommandExecutionOutcome outcome, bool terminationConfirmed,
                std::optional<int> exitCode = std::nullopt);

    SessionInputArbiter* _arbiter{nullptr};
    InteractiveStreamFramer* _framer{nullptr};
    QTimer _timeout;
    std::optional<CommandExecutionRequest> _active;
    QByteArray _standardOutput;
    quint64 _sessionGeneration{0};
    quint64 _promptGeneration{0};
    bool _executionMayHaveStarted{false};
    bool _commandStarted{false};
    bool _outputTruncated{false};
};

