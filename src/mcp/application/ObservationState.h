/** @file ObservationState.h @brief 终端观察等待与输入重放的应用所有者。 */
#pragma once

#include "CaptureStore.h"
#include "mcp/contracts/Budget.h"
#include "mcp/contracts/Errors.h"
#include "mcp/contracts/ToolRequests.h"

#include <QHash>
#include <QList>
#include <QSet>

#include <functional>
#include <memory>

namespace NovaTerm::Mcp {

/** @brief 签名验证和字段解码由入口完成；绑定比较由应用完成。 */
struct ObservationBinding
{
    bool verified{false};
    ClientId client;
    SessionReference session;
    ClientAccessVersion grant;
};

struct ObservationCursor
{
    ObservationBinding binding;
    quint64 model{0};
    quint64 event{0};
    bool legacy{false};
    bool includeViewport{true};
    quint64 cacheGeneration{0};
};

struct TerminalControlFacts
{
    SessionReference session;
    ClientAccessVersion grant;
    quint64 user{0};
    quint64 control{0};
    quint64 prompt{0};
    ExecutionId execution;
    ClientId owner;
    bool commandActive{false};
    bool readyForCommand{false};
    bool integrated{false};
    bool pending{false};
    int controlledSessions{0};
    std::optional<ErrorCode> unavailable;
    quint64 cacheGeneration{0};
};

struct InputClaims
{
    ObservationBinding binding;
    quint64 user{0};
    quint64 control{0};
    quint64 prompt{0};
    ExecutionId execution;
    quint64 sequence{0};
    qint64 expires{0};
    quint64 cacheGeneration{0};
};

enum class InputAdmission { Replay, Submit, Release };

enum class ObservationWake
{
    Initial, Deadline, StateChanged, ControlChanged, CommandFinished,
    OutputChanged, AttentionCandidate, DiagnosticCandidate,
};

/** @brief 只有应用所有者修改等待状态；适配器只消费不可变完成投影。 */
struct ObservationWait
{
    ClientId client;
    RequestId request;
    SessionReference session;
    ToolRequest typed;
    quint64 since{0};
    quint64 event{0};
    quint64 user{0};
    quint64 control{0};
    ClientAccessVersion grant;
    bool initial{false};
    bool interaction{false};
    std::optional<ObservationWake> immediate;
    quint64 cacheGeneration{0};
};

struct ObservationFacts
{
    bool available{false};
    SessionReference session;
    ClientAccessVersion grant;
    bool snapshotAvailable{false};
    quint64 model{0};
    quint64 event{0};
    quint64 user{0};
    quint64 control{0};
    bool running{false};
    bool commandFinished{false};
    QString lastVisibleLine;
    quint64 cacheGeneration{0};
};

struct ObservationCompletion
{
    ObservationWait wait;
    std::optional<Error> error;
    ObservationWake wake{ObservationWake::Deadline};
};

class ObservationState
{
public:
    using Clock = std::function<qint64()>;
    // true commits accepted input; false rolls back only this pending reservation.
    // The closure is safe even if its ObservationState owner has been destroyed.
    using InputReservation = std::function<bool(bool)>;
    explicit ObservationState(Clock clock);
    ObservationState(const ObservationState&) = delete;
    ObservationState& operator=(const ObservationState&) = delete;

    [[nodiscard]] bool matches(const ObservationBinding& binding, const ClientId& client,
        const SessionReference& session, ClientAccessVersion grant) const;
    [[nodiscard]] Result<ObservationCursor> restoreCursor(const ObservationCursor& cursor,
        const ClientId& client, const SessionReference& session, ClientAccessVersion grant,
        bool includeViewport, quint64 cacheGeneration = 0) const;
    [[nodiscard]] InputClaims inputClaims(const ClientId& client, const TerminalControlFacts& facts) const;
    [[nodiscard]] std::optional<Error> validateInput(const InputClaims& claims,
        const ClientId& client, const TerminalControlFacts& facts) const;
    [[nodiscard]] Result<InputAdmission> prepareInput(const ClientId& client,
        const InteractTerminalRequest& request, const InputClaims& claims,
        const TerminalControlFacts& facts) const;
    [[nodiscard]] Result<InputReservation> reserveInput(const ClientId& client,
        const InteractTerminalRequest& request, const TerminalControlFacts& facts);
    void inputAccepted(const ClientId& client, const InteractTerminalRequest& request,
        const TerminalControlFacts& facts);

    [[nodiscard]] bool canAdmit(const ClientId& client, const SessionId& session) const;
    [[nodiscard]] Result<QList<ObservationCompletion>> admit(ObservationWait wait);
    [[nodiscard]] QList<ObservationWait> waiters() const;
    [[nodiscard]] std::optional<ObservationCompletion> evaluate(const RequestId& request,
        const ObservationFacts& facts);
    void outputChanged(const SessionId& session, quint64 model);
    bool cancel(const RequestId& request);
    [[nodiscard]] QList<ObservationCompletion> revoke(const ClientId& client, const SessionId& session);
    [[nodiscard]] bool hasWaiters() const noexcept { return !_waiting.isEmpty(); }
    [[nodiscard]] qsizetype waiterCount() const noexcept { return _waiting.size(); }
    [[nodiscard]] bool captureAvailable(const CaptureRecord& capture, const ClientId& client,
        const SessionReference& session, ClientAccessVersion grant, quint64 cacheGeneration = 0) const;

private:
    struct Scope
    {
        ClientId client;
        SessionId session;
        friend bool operator==(const Scope& a, const Scope& b)
        { return a.client == b.client && a.session == b.session; }
        friend size_t qHash(const Scope& value, size_t seed = 0) noexcept
        { return qHashMulti(seed, value.client, value.session); }
    };
    struct Replay
    {
        quint64 generation{0};
        quint64 cacheGeneration{0};
        quint64 next{0};
        Epoch epoch;
        ClientAccessVersion grant;
        OperationId operation;
        QByteArray identity;
        quint64 serial{0};
    };
    struct Waiting
    {
        ObservationWait wait;
        qint64 deadline{0};
        WaitCondition mode{WaitCondition::Attention};
    };
    struct Activity { quint64 model{0}; qint64 outputAt{0}; };

    [[nodiscard]] std::optional<ObservationWake> attention(const ObservationWait& wait, const QString& line);
    Clock _clock;
    QList<Waiting> _waiting;
    QHash<Scope, Replay> _replays;
    struct InputHistory;
    std::shared_ptr<InputHistory> _inputs;
    QHash<Scope, QSet<QByteArray>> _hints;
    QHash<SessionId, Activity> _activity;
};

} // namespace NovaTerm::Mcp
