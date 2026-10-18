/**
 * @file SessionCommandCoordinator.h
 * @brief Session 交互命令的提示符、Lease、捕获与完成状态机。
 */
#pragma once

#include "CommandExecutionTypes.h"
#include "InteractiveStreamFramer.h"
#include "session/automation/TerminalEvidenceModel.h"

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
    /** @brief 终端收到其他输入后废弃此前提示符证据，不影响活动命令。 */
    void invalidatePromptReadiness();
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
        return _evidence.promptGeneration();
    }
    [[nodiscard]] quint64 promptUserGeneration() const noexcept
    {
        return _evidence.promptUserGeneration();
    }
    /** @brief 仅用于控制器的编码失败回滚；不重新验证候选/静默窗。 */
    void restorePromptEvidence(quint64 generation, quint64 userGeneration);
    /** @brief 已知提示符证据（控制器事件路径）；幂等写入单一真源。 */
    void notePromptReady(quint64 generation, quint64 userGeneration);

public slots:
    void handleInteractiveEvent(const InteractiveStreamEvent& event);
    void handleInteractiveBytes(const QByteArray& bytes);

signals:
    void finished(const CommandExecutionResult& result);
    /** @brief 候选提示符静默窗结束、成为可提交证据。 */
    void promptReady(quint64 generation);
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
    NovaTerm::Session::TerminalEvidenceModel _evidence;
    InteractiveCommandProfile _profile;
    std::optional<CommandExecutionRequest> _active;
    QByteArray _pendingInput;
    qsizetype _pendingInputOffset{0};
    QByteArray _standardOutput;
    quint64 _sessionGeneration{0};
    bool _executionMayHaveStarted{false};
    bool _commandStarted{false};
    bool _outputTruncated{false};
    static constexpr qsizetype InputChunkBytes = 512;
};
