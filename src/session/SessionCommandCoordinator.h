/**
 * @file SessionCommandCoordinator.h
 * @brief Session 提示符候选、静默窗与就绪证据。
 */
#pragma once

#include "InteractiveStreamFramer.h"
#include "session/automation/TerminalEvidenceModel.h"

#include <QByteArray>
#include <QObject>

class SessionInputArbiter;

/** @brief 在 Session 层维护当前提示符证据，供终端控制器读取。 */
class SessionCommandCoordinator final : public QObject
{
    Q_OBJECT
public:
    explicit SessionCommandCoordinator(SessionInputArbiter* arbiter, QObject* parent = nullptr);

    void reset();
    void configure(InteractiveCommandProfile profile);
    /** @brief 终端收到其他输入后废弃此前提示符证据。 */
    void invalidatePromptReadiness();
    [[nodiscard]] bool isPromptReady() const noexcept;
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
    /** @brief 候选提示符静默窗结束、成为可提交证据。 */
    void promptReady(quint64 generation);

private:
    SessionInputArbiter* _arbiter{nullptr};
    NovaTerm::Session::TerminalEvidenceModel _evidence;
    bool _shellIntegration{false};
};
