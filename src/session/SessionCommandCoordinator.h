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
    void configure(InteractiveCommandProfile profile);
    [[nodiscard]] bool submit(const CommandExecutionRequest& request);
    void cancel(quint64 requestId);
    void expire(quint64 requestId);
    [[nodiscard]] bool isPromptReady() const noexcept;
    /** @brief 当前 Session 是否安装了显式可信交互 Profile。 */
    [[nodiscard]] bool hasTrustedProfile() const noexcept
    {
        return !_profile.allowUnverifiedPrompt
            && (_profile.shellIntegration || !_profile.promptPattern.isEmpty());
    }
    [[nodiscard]] bool allowsUnverifiedPrompt() const noexcept
    {
        return _profile.allowUnverifiedPrompt;
    }
    [[nodiscard]] quint64 promptGeneration() const noexcept
    {
        return _promptGeneration;
    }

public slots:
    void handleInteractiveEvent(const InteractiveStreamEvent& event);
    void handleInteractiveBytes(const QByteArray& bytes);

signals:
    void finished(const CommandExecutionResult& result);
    /** @brief 将取消时尚未完成匹配的普通回显交还输入泵。 */
    void visibleRemainder(const QByteArray& bytes);

private:
    [[nodiscard]] bool sendNextInputChunk();
    void handlePreempted(quint64 executionId, bool executionMayHaveStarted);
    void finish(CommandExecutionOutcome outcome, bool terminationConfirmed,
                std::optional<int> exitCode = std::nullopt);

    SessionInputArbiter* _arbiter{nullptr};
    InteractiveStreamFramer* _framer{nullptr};
    QTimer _timeout;
    QTimer _promptSilence;
    InteractiveCommandProfile _profile;
    std::optional<CommandExecutionRequest> _active;
    QByteArray _pendingInput;
    qsizetype _pendingInputOffset{0};
    QByteArray _standardOutput;
    quint64 _sessionGeneration{0};
    quint64 _promptGeneration{0};
    quint64 _candidatePromptGeneration{0};
    quint64 _promptUserInputGeneration{0};
    quint64 _candidateUserInputGeneration{0};
    bool _executionMayHaveStarted{false};
    bool _commandStarted{false};
    bool _outputTruncated{false};
    static constexpr qsizetype InputChunkBytes = 512;
};
