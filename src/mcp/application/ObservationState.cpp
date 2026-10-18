/** @file ObservationState.cpp @brief 类型化输入准入、等待唤醒与失效规则。 */
#include "ObservationState.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QRegularExpression>

#include <algorithm>
#include <type_traits>

namespace NovaTerm::Mcp {
namespace {
bool commandAction(const ToolRequest& request)
{
    const auto* input = std::get_if<InteractTerminalRequest>(&request);
    return input && !input->actions.empty()
        && std::holds_alternative<CommandAction>(input->actions.front());
}

QByteArray inputIdentity(const InteractTerminalRequest& request)
{
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream << quint32(request.actions.size());
    for (const auto& action : request.actions) {
        stream << quint8(action.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, TextAction> || std::is_same_v<T, PasteAction>)
                stream << value.text;
            else if constexpr (std::is_same_v<T, CommandAction>)
                stream << value.command;
            else if constexpr (std::is_same_v<T, KeyAction>) {
                stream << value.key << quint32(value.modifiers.size());
                for (const auto modifier : value.modifiers) stream << quint8(modifier);
            }
        }, action);
    }
    stream << request.replyToExecution.has_value();
    if (request.replyToExecution) stream << request.replyToExecution->str();
    stream << request.requireVerifiedReady;
    return QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
}

std::pair<qint64, WaitCondition> waitPolicy(const ObservationWait& wait)
{
    std::optional<std::chrono::milliseconds> duration;
    std::optional<WaitCondition> condition;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ReadContextRequest>
            || std::is_same_v<T, InteractTerminalRequest>
            || std::is_same_v<T, RequestElevationRequest>) {
            duration = value.wait;
            condition = value.waitFor;
        }
    }, wait.typed);
    const auto fallback = wait.interaction
        ? (commandAction(wait.typed) ? Budget::MaxTerminalWaitMs : 1000) : 0;
    const auto milliseconds = std::clamp(duration ? duration->count() : qint64(fallback),
        qint64(0), qint64(Budget::MaxTerminalWaitMs));
    const auto mode = condition.value_or(wait.interaction && !commandAction(wait.typed)
        ? WaitCondition::Change : WaitCondition::Attention);
    const auto* input = std::get_if<InteractTerminalRequest>(&wait.typed);
    const bool release = input && !input->actions.empty()
        && std::holds_alternative<ReleaseAction>(input->actions.front());
    return {wait.initial || wait.immediate || release ? 0 : milliseconds, mode};
}
} // namespace

ObservationState::ObservationState(Clock clock) : _clock(std::move(clock)) {}

bool ObservationState::matches(const ObservationBinding& binding, const ClientId& client,
    const SessionReference& session, ClientAccessVersion grant) const
{
    return binding.verified && binding.client == client && binding.session.session == session.session
        && binding.session.epoch == session.epoch && binding.grant == grant;
}

Result<ObservationCursor> ObservationState::restoreCursor(const ObservationCursor& cursor,
    const ClientId& client, const SessionReference& session, ClientAccessVersion grant,
    bool includeViewport) const
{
    if (!matches(cursor.binding, client, session, grant)
        || (cursor.legacy && cursor.includeViewport != includeViewport))
        return Error{ErrorCode::CursorStale};
    return cursor;
}

InputClaims ObservationState::inputClaims(const ClientId& client, const TerminalControlFacts& facts) const
{
    InputClaims claims;
    claims.binding = {true, client, facts.session, facts.grant};
    claims.user = facts.user;
    claims.control = facts.control;
    claims.prompt = facts.prompt;
    claims.execution = facts.execution;
    const auto replay = _replays.constFind({client, facts.session.session});
    if (replay != _replays.cend() && replay->generation == facts.control
        && replay->epoch == facts.session.epoch && replay->grant == facts.grant)
        claims.sequence = replay->next;
    claims.expires = _clock() + Budget::MaxTerminalWaitMs;
    return claims;
}

std::optional<Error> ObservationState::validateInput(const InputClaims& claims,
    const ClientId& client, const TerminalControlFacts& facts) const
{
    if (!matches(claims.binding, client, facts.session, facts.grant))
        return Error{ErrorCode::ControlInvalid};
    if (claims.expires <= _clock()) return Error{ErrorCode::InputTokenExpired};
    const auto expected = inputClaims(client, facts);
    if (claims.user != expected.user || claims.control != expected.control
        || claims.prompt != expected.prompt || claims.execution != expected.execution)
        return Error{ErrorCode::ControlInvalid};
    if (claims.sequence < expected.sequence) return Error{ErrorCode::InputAlreadySubmitted};
    if (claims.sequence > expected.sequence) return Error{ErrorCode::InputOutOfOrder};
    return std::nullopt;
}

Result<InputAdmission> ObservationState::prepareInput(const ClientId& client,
    const InteractTerminalRequest& request, const InputClaims& claims,
    const TerminalControlFacts& facts) const
{
    if (request.session != facts.session.session || request.epoch != facts.session.epoch)
        return Error{ErrorCode::ControlInvalid};
    if (facts.unavailable) return Error{*facts.unavailable};
    const auto replay = _replays.constFind({client, facts.session.session});
    if (replay != _replays.cend() && replay->operation == request.operation
        && replay->epoch == facts.session.epoch && replay->grant == facts.grant
        && replay->generation == facts.control) {
        if (replay->identity != inputIdentity(request))
            return Error{ErrorCode::InputSequenceConflict};
        return InputAdmission::Replay;
    }
    if (const auto error = validateInput(claims, client, facts)) return *error;
    const bool release = !request.actions.empty() && std::holds_alternative<ReleaseAction>(request.actions.front());
    const bool command = !request.actions.empty() && std::holds_alternative<CommandAction>(request.actions.front());
    if ((request.replyToExecution && (!facts.commandActive || facts.execution != *request.replyToExecution))
        || (facts.commandActive && !release && !request.replyToExecution))
        return Error{ErrorCode::CommandContextChanged};
    if (command && (!facts.readyForCommand || (request.requireVerifiedReady && !facts.integrated)))
        return Error{ErrorCode::ContextNotReady};
    if (facts.pending)
        return Error{ErrorCode::InputBusy, {}, RetryHint{true, std::chrono::milliseconds(50)}};
    if (release) return InputAdmission::Release;
    if (facts.controlledSessions >= Budget::MaxObservedControlSessions && facts.owner.isEmpty())
        return Error{ErrorCode::Busy};
    if (!facts.owner.isEmpty() && facts.owner != client)
        return Error{ErrorCode::TerminalBusy};
    return InputAdmission::Submit;
}

void ObservationState::inputAccepted(const ClientId& client, const InteractTerminalRequest& request,
    const TerminalControlFacts& facts)
{
    auto& replay = _replays[{client, facts.session.session}];
    if (replay.generation != facts.control || replay.epoch != facts.session.epoch || replay.grant != facts.grant)
        replay.next = 0;
    replay.generation = facts.control;
    ++replay.next;
    replay.epoch = facts.session.epoch;
    replay.grant = facts.grant;
    replay.operation = request.operation;
    replay.identity = inputIdentity(request);
    _hints.remove({client, facts.session.session});
}

bool ObservationState::canAdmit(const ClientId& client, const SessionId& session) const
{
    return _waiting.size() < Budget::MaxObservationWaiters
        || std::any_of(_waiting.cbegin(), _waiting.cend(), [&](const Waiting& wait) {
            return wait.wait.client == client && wait.wait.session.session == session;
        });
}

Result<QList<ObservationCompletion>> ObservationState::admit(ObservationWait wait)
{
    if (!canAdmit(wait.client, wait.session.session)) return Error{ErrorCode::Busy};
    if (std::any_of(_waiting.cbegin(), _waiting.cend(), [&](const Waiting& existing) {
        return existing.wait.request == wait.request;
    })) return Error{ErrorCode::Busy};
    QList<ObservationCompletion> replaced;
    for (auto it = _waiting.begin(); it != _waiting.end();) {
        if (it->wait.client == wait.client && it->wait.session.session == wait.session.session) {
            const bool sameBinding = it->wait.session.epoch == wait.session.epoch
                && it->wait.grant == wait.grant;
            replaced.append({std::move(it->wait), sameBinding ? std::nullopt
                : std::optional<Error>{Error{ErrorCode::SessionNotAvailable}}, ObservationWake::StateChanged});
            it = _waiting.erase(it);
        } else ++it;
    }
    wait.interaction = !std::holds_alternative<ReadContextRequest>(wait.typed);
    const auto [duration, mode] = waitPolicy(wait);
    _waiting.append({std::move(wait), _clock() + duration, mode});
    return replaced;
}

QList<ObservationWait> ObservationState::waiters() const
{
    QList<ObservationWait> result;
    for (const auto& wait : _waiting) result.append(wait.wait);
    return result;
}

std::optional<ObservationCompletion> ObservationState::evaluate(const RequestId& request,
    const ObservationFacts& facts)
{
    const auto it = std::find_if(_waiting.begin(), _waiting.end(), [&](const Waiting& entry) {
        return entry.wait.request == request;
    });
    if (it == _waiting.end()) return std::nullopt;
    const auto& wait = it->wait;
    std::optional<Error> error;
    auto wake = wait.immediate;
    const auto now = _clock();
    if (!facts.available || facts.session.session != wait.session.session
        || facts.session.epoch != wait.session.epoch || facts.grant != wait.grant)
        error = Error{ErrorCode::SessionNotAvailable};
    else if (!facts.snapshotAvailable) {
        if (now < it->deadline) return std::nullopt;
        error = Error{ErrorCode::Busy, {}, RetryHint{true, std::chrono::milliseconds(50)}};
    } else {
        const bool event = wait.event != facts.event;
        const bool model = wait.since != facts.model;
        if (!wake && (wait.user != facts.user || wait.control != facts.control || !facts.running))
            wake = ObservationWake::ControlChanged;
        if (!wake && event && facts.commandFinished) wake = ObservationWake::CommandFinished;
        const bool quiet = now - _activity.value(wait.session.session).outputAt >= 150;
        if (!wake && it->mode == WaitCondition::Change && (model || event) && quiet)
            wake = ObservationWake::OutputChanged;
        if (!wake && it->mode == WaitCondition::Attention && model && quiet)
            wake = attention(wait, facts.lastVisibleLine);
        if (!wake && now >= it->deadline)
            wake = wait.initial ? ObservationWake::Initial : ObservationWake::Deadline;
    }
    if (!wake && !error) return std::nullopt;
    ObservationCompletion completed{std::move(it->wait), std::move(error), wake.value_or(ObservationWake::Deadline)};
    _waiting.erase(it);
    return completed;
}

void ObservationState::outputChanged(const SessionId& session, quint64 model)
{
    auto& activity = _activity[session];
    if (activity.model != model) {
        activity.model = model;
        activity.outputAt = _clock();
    }
}

std::optional<ObservationWake> ObservationState::attention(const ObservationWait& wait, const QString& line)
{
    auto& delivered = _hints[{wait.client, wait.session.session}];
    if (delivered.size() >= 4) return std::nullopt;
    static const QRegularExpression prompt(QStringLiteral("(?i)(\\[[yn]/[yn]\\]|\\([yn]/[yn]\\)|(?:password|passphrase):\\s*$|(?:continue|proceed).{0,32}\\?|press .{0,32}key)"));
    static const QRegularExpression error(QStringLiteral("(?i)^(?:fatal error:|error:|traceback|panic:)|: fatal error:"));
    const auto reason = prompt.match(line).hasMatch() ? ObservationWake::AttentionCandidate
        : error.match(line).hasMatch() ? ObservationWake::DiagnosticCandidate : ObservationWake::Deadline;
    if (reason == ObservationWake::Deadline) return std::nullopt;
    const auto fingerprint = QCryptographicHash::hash(
        (QString::number(int(reason)) + line).toUtf8(), QCryptographicHash::Sha256);
    if (delivered.contains(fingerprint)) return std::nullopt;
    delivered.insert(fingerprint);
    return reason;
}

bool ObservationState::cancel(const RequestId& request)
{
    const auto it = std::find_if(_waiting.begin(), _waiting.end(), [&](const Waiting& wait) {
        return wait.wait.request == request;
    });
    if (it == _waiting.end()) return false;
    _waiting.erase(it);
    return true;
}

QList<ObservationCompletion> ObservationState::revoke(const ClientId& client, const SessionId& session)
{
    const auto selected = [&](const ClientId& owner, const SessionId& id) {
        return (client.isEmpty() || client == owner) && (session.isEmpty() || session == id);
    };
    QList<ObservationCompletion> result;
    for (auto it = _waiting.begin(); it != _waiting.end();) {
        if (selected(it->wait.client, it->wait.session.session)) {
            result.append({std::move(it->wait), Error{ErrorCode::SessionNotAvailable}, ObservationWake::Deadline});
            it = _waiting.erase(it);
        } else ++it;
    }
    for (auto it = _replays.begin(); it != _replays.end();) {
        if (selected(it.key().client, it.key().session)) it = _replays.erase(it); else ++it;
    }
    for (auto it = _hints.begin(); it != _hints.end();) {
        if (selected(it.key().client, it.key().session)) it = _hints.erase(it); else ++it;
    }
    if (!session.isEmpty()) _activity.remove(session);
    else if (client.isEmpty()) _activity.clear();
    return result;
}

bool ObservationState::captureAvailable(const CaptureRecord& capture, const ClientId& client,
    const SessionReference& session, ClientAccessVersion grant) const
{
    return capture.scope == CaptureScope::Client && capture.owner == client.str()
        && capture.session == session.session.str() && capture.epoch == session.epoch.str()
        && capture.grantVersion == grant.value() && capture.expires > _clock();
}

} // namespace NovaTerm::Mcp
