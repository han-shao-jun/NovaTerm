/** @file ObservationState.cpp @brief 类型化输入准入、等待唤醒与失效规则。 */
#include "ObservationState.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QRegularExpression>

#include <algorithm>
#include <type_traits>
#include <limits>

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

struct ObservationState::InputHistory
{
    struct Key {
        Scope scope;
        Epoch epoch;
        ClientAccessVersion grant;
        quint64 cache{0};
        OperationId operation;
        friend bool operator==(const Key& a, const Key& b) {
            return a.scope == b.scope && a.epoch == b.epoch && a.grant == b.grant
                && a.cache == b.cache && a.operation == b.operation;
        }
        friend size_t qHash(const Key& value, size_t seed = 0) noexcept {
            return qHashMulti(seed, value.scope, value.epoch, value.grant.value(), value.cache, value.operation);
        }
    };
    struct Entry { quint64 serial{0}; qint64 expires{0}; bool accepted{false}; };
    QHash<Key, Entry> records;
    QHash<Scope, qsizetype> counts;
    quint64 nextSerial{0};
    qint64 nextExpiry{std::numeric_limits<qint64>::max()};

    static Key key(const ClientId& client, const InteractTerminalRequest& request,
        const TerminalControlFacts& facts) {
        return {{client, request.session}, facts.session.epoch, facts.grant,
            facts.cacheGeneration, request.operation};
    }
    QHash<Key, Entry>::iterator erase(QHash<Key, Entry>::iterator it) {
        const auto scope = it.key().scope;
        auto count = counts.find(scope);
        if (count != counts.end() && --count.value() == 0) counts.erase(count);
        return records.erase(it);
    }
    void prune(qint64 now) {
        if (now < nextExpiry) return;
        nextExpiry = std::numeric_limits<qint64>::max();
        for (auto it = records.begin(); it != records.end();) {
            if (it->accepted && it->expires <= now) it = erase(it);
            else { if (it->accepted) nextExpiry = std::min(nextExpiry, it->expires); ++it; }
        }
    }
    bool room(const Scope& scope, bool releaseOwned) const {
        // One emergency release per controlled scope remains possible when full.
        return counts.value(scope) < Budget::MaxTerminalInputRecordsPerScope + (releaseOwned ? 1 : 0)
            && records.size() < Budget::MaxTerminalInputRecords
                + (releaseOwned ? Budget::MaxObservedControlSessions : 0);
    }
};

ObservationState::ObservationState(Clock clock)
    : _clock(std::move(clock)), _inputs(std::make_shared<InputHistory>()) {}

bool ObservationState::matches(const ObservationBinding& binding, const ClientId& client,
    const SessionReference& session, ClientAccessVersion grant) const
{
    return binding.verified && binding.client == client && binding.session.session == session.session
        && binding.session.epoch == session.epoch && binding.grant == grant;
}

Result<ObservationCursor> ObservationState::restoreCursor(const ObservationCursor& cursor,
    const ClientId& client, const SessionReference& session, ClientAccessVersion grant,
    bool includeViewport, quint64 cacheGeneration) const
{
    if (!matches(cursor.binding, client, session, grant)
        || cursor.cacheGeneration != cacheGeneration
        || (cursor.legacy && cursor.includeViewport != includeViewport))
        return Error{ErrorCode::CursorStale};
    return cursor;
}

InputClaims ObservationState::inputClaims(const ClientId& client, const TerminalControlFacts& facts) const
{
    InputClaims claims;
    claims.binding = {true, client, facts.session, facts.grant};
    claims.cacheGeneration = facts.cacheGeneration;
    claims.user = facts.user;
    claims.control = facts.control;
    claims.prompt = facts.prompt;
    claims.execution = facts.execution;
    const auto replay = _replays.constFind({client, facts.session.session});
    if (replay != _replays.cend() && replay->generation == facts.control
        && replay->cacheGeneration == facts.cacheGeneration
        && replay->epoch == facts.session.epoch && replay->grant == facts.grant)
        claims.sequence = replay->next;
    claims.expires = _clock() + Budget::MaxTerminalWaitMs;
    return claims;
}

std::optional<Error> ObservationState::validateInput(const InputClaims& claims,
    const ClientId& client, const TerminalControlFacts& facts) const
{
    if (!matches(claims.binding, client, facts.session, facts.grant)
        || claims.cacheGeneration != facts.cacheGeneration)
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
    if (claims.cacheGeneration != facts.cacheGeneration)
        return Error{ErrorCode::ControlInvalid};
    _inputs->prune(_clock());
    const auto known = _inputs->records.constFind(InputHistory::key(client, request, facts));
    if (known != _inputs->records.cend() && !known->accepted)
        return Error{ErrorCode::InputBusy, {}, RetryHint{true, std::chrono::milliseconds(50)}};
    const auto replay = _replays.constFind({client, facts.session.session});
    if (replay != _replays.cend() && replay->operation == request.operation
        && replay->epoch == facts.session.epoch && replay->grant == facts.grant
        && replay->cacheGeneration == facts.cacheGeneration
        && replay->generation == facts.control) {
        if (replay->identity != inputIdentity(request))
            return Error{ErrorCode::InputSequenceConflict};
        return InputAdmission::Replay;
    }
    if (known != _inputs->records.cend())
        return Error{ErrorCode::InputSequenceConflict,
            "operationId already identifies an earlier accepted input; use a new ID for a new intent."};
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
    if (!_inputs->room({client, request.session}, release && facts.owner == client))
        return Error{ErrorCode::Busy, "Recent input identity budget is full; no input was submitted."};
    if (release) return InputAdmission::Release;
    if (facts.controlledSessions >= Budget::MaxObservedControlSessions && facts.owner.isEmpty())
        return Error{ErrorCode::Busy};
    if (!facts.owner.isEmpty() && facts.owner != client)
        return Error{ErrorCode::TerminalBusy};
    return InputAdmission::Submit;
}

Result<ObservationState::InputReservation> ObservationState::reserveInput(const ClientId& client,
    const InteractTerminalRequest& request, const TerminalControlFacts& facts)
{
    _inputs->prune(_clock());
    const auto key = InputHistory::key(client, request, facts);
    if (_inputs->records.contains(key)) return Error{ErrorCode::InputBusy};
    const bool releasing = !request.actions.empty()
        && std::holds_alternative<ReleaseAction>(request.actions.front()) && facts.owner == client;
    if (!_inputs->room(key.scope, releasing)) return Error{ErrorCode::Busy};
    const auto serial = ++_inputs->nextSerial;
    _inputs->records.insert(key, {serial, 0, false});
    ++_inputs->counts[key.scope];
    const std::weak_ptr<InputHistory> history = _inputs;
    return InputReservation{[history, key, serial, clock = _clock](bool accepted) {
        const auto owned = history.lock();
        if (!owned) return false;
        auto it = owned->records.find(key);
        if (it == owned->records.end() || it->serial != serial) return false;
        if (it->accepted) return true; // Replay/duplicate completion never extends retention.
        if (!accepted) { owned->erase(it); return false; }
        it->accepted = true;
        it->expires = clock() + Budget::TerminalInputRecordRetention.count();
        owned->nextExpiry = std::min(owned->nextExpiry, it->expires);
        return true;
    }};
}

void ObservationState::inputAccepted(const ClientId& client, const InteractTerminalRequest& request,
    const TerminalControlFacts& facts)
{
    const auto key = InputHistory::key(client, request, facts);
    auto accepted = _inputs->records.constFind(key);
    // Direct application-state users also report an already accepted operation.
    if (accepted == _inputs->records.cend()) {
        auto reserved = reserveInput(client, request, facts);
        if (isError(reserved)) return;
        std::get<InputReservation>(reserved)(true);
        accepted = _inputs->records.constFind(key);
    }
    if (accepted == _inputs->records.cend() || !accepted->accepted) return;
    auto& replay = _replays[{client, facts.session.session}];
    // A synchronous controller callback may have admitted a newer operation.
    if (replay.serial >= accepted->serial) return;
    replay.serial = accepted->serial;
    if (replay.generation != facts.control || replay.cacheGeneration != facts.cacheGeneration
        || replay.epoch != facts.session.epoch || replay.grant != facts.grant)
        replay.next = 0;
    replay.generation = facts.control;
    replay.cacheGeneration = facts.cacheGeneration;
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
    else if (facts.cacheGeneration != wait.cacheGeneration)
        error = Error{ErrorCode::CursorStale};
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
    for (auto it = _inputs->records.begin(); it != _inputs->records.end();) {
        if (selected(it.key().scope.client, it.key().scope.session)) {
            it = _inputs->erase(it);
        } else ++it;
    }
    for (auto it = _hints.begin(); it != _hints.end();) {
        if (selected(it.key().client, it.key().session)) it = _hints.erase(it); else ++it;
    }
    if (!session.isEmpty()) _activity.remove(session);
    else if (client.isEmpty()) _activity.clear();
    return result;
}

bool ObservationState::captureAvailable(const CaptureRecord& capture, const ClientId& client,
    const SessionReference& session, ClientAccessVersion grant, quint64 cacheGeneration) const
{
    return capture.scope == CaptureScope::Client && capture.owner == client.str()
        && capture.session == session.session.str() && capture.epoch == session.epoch.str()
        && capture.grantVersion == grant.value() && capture.expires > _clock()
        && capture.cacheGeneration == cacheGeneration;
}

} // namespace NovaTerm::Mcp
