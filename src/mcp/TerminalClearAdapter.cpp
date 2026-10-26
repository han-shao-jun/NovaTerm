#include "TerminalClearAdapter.h"
#include "LogReadAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "protocol/ToolCodec.h"
#include "session/SessionInputArbiter.h"
#include <QPointer>
#include <QDateTime>

namespace NovaTerm::Mcp {
TerminalClearAdapter::TerminalClearAdapter(AccessStore& access, SessionDirectory& sessions,
    QString instance, QByteArray key, QObject* parent)
    : QObject(parent), _access(access), _sessions(sessions), _instance(std::move(instance)), _key(std::move(key))
{
    connect(&_sessions, &SessionDirectory::invalidated, this, [this](const QString& id) {
        _operations.cancelSession({}, id);
    });
    connect(&_access, &AccessStore::revoked, this, [this](const QString& client, const QString& session) {
        _operations.cancelSession(client, session);
    });
}
TerminalClearAdapter::~TerminalClearAdapter() { _operations.cancelSession({}, {}); }
quint64 TerminalClearAdapter::watch(TerminalSession* session)
{
    if (_watched.contains(session)) return _watched.value(session);
    const auto source = ++_nextSource;
    _watched.insert(session, source);
    connect(session, &TerminalSession::terminalClearCompleted, this,
        [this](quint64 operation, quint64 generation, int status, quint64 nextLine) { finished(operation, generation, status, nextLine); });
    connect(session->inputArbiter(), &SessionInputArbiter::userInputStarted, this,
        [this, source] {
            for (const auto& record : _operations.records())
                if (record->source == source && !record->result) record->cancelled->store(true);
        });
    connect(session, &QObject::destroyed, this, [this, session, source] {
        _watched.remove(session);
        const auto records = _operations.records();
        for (const auto& record : records) if (record->source == source && !record->result) {
            record->cancelled->store(true);
            _operations.finish(record->id, Error{ErrorCode::SessionNotAvailable});
        }
    });
    return source;
}
void TerminalClearAdapter::call(const QString& client, const QString& requestKey,
    const ClearRequest& request, Completion done)
{
    const auto fail = [&done](ErrorCode code) { done(ToolCodec::encodeFailure(Error{code})); };
    if (request.operation.isEmpty() || !request.expectedGeneration) { fail(ErrorCode::InvalidArgument); return; }
    const auto entry = _sessions.find(request.session.str());
    if (!entry || !entry->session || !_access.canRead(client, *entry)) { fail(ErrorCode::SessionNotAvailable); return; }
    if (entry->epoch != request.epoch.str()) { fail(ErrorCode::StaleSessionEpoch); return; }
    if (!_access.canClearTerminal(client, *entry)) { fail(ErrorCode::PermissionDenied); return; }
    const auto grant = _access.version(client);
    const auto ticket = verifyToken(request.clearToken, _key);
    if (!ticket || ticket->value("kind") != "clear" || ticket->value("version") != 1
        || ticket->value("client") != client || ticket->value("session") != request.session.str()
        || ticket->value("epoch") != request.epoch.str() || ticket->value("grant") != QString::number(grant)
        || ticket->value("generation") != QString::number(request.expectedGeneration)) {
        fail(ErrorCode::CursorStale); return;
    }
    bool valid = false;
    const auto issued = ticket->value("issued").toString().toLongLong(&valid);
    const auto now = clearTicketClock();
    if (!valid || issued > now || issued < 0 || now - issued >= ClearTicketLifetimeMs) { fail(ErrorCode::CursorStale); return; }
    const auto result = _operations.begin(client, request, grant, issued + ClearTicketLifetimeMs, now);
    if (const auto* error = errorIfAny(result)) { done(ToolCodec::encodeFailure(*error)); return; }
    const auto admitted = std::get<ClearOperations::Admission>(result);
    const auto record = admitted.record;
    const QPointer<TerminalClearAdapter> guard(this);
    if (!_operations.wait(record, requestKey, [this, guard, client, request, grant, done = std::move(done)](auto value) {
        if (!guard) return;
        const auto current = _sessions.find(request.session.str());
        if (!current || !current->session || current->epoch != request.epoch.str()
            || !_access.canClearTerminal(client, *current) || _access.version(client) != grant) {
            done(ToolCodec::encodeFailure(Error{ErrorCode::SessionNotAvailable})); return;
        }
        if (const auto* error = errorIfAny(value)) { done(ToolCodec::encodeFailure(*error)); return; }
        const auto& completion = std::get<ClearCompletion>(value);
        QJsonObject data{{"operationId", request.operation.str()}, {"scope", request.all ? "all" : "scrollback"},
            {"status", "applied"}, {"cacheGeneration", QString::number(completion.generation)},
            {"baseline", QJsonValue::Null}};
        if (completion.baseline) data.insert("baseline", encodeLogPage(*completion.baseline, _instance, client,
            {request.session, request.epoch}, grant, _key, std::nullopt, completion.capturedAtMs, completion.ticketIssued,
            completion.userGeneration));
        done(success(data));
    })) return; // Request budgets already bound concurrent waiters; no parser submission on overflow.
    if (!admitted.fresh) return;
    bool userValid = false;
    const auto user = ticket->value("user").toString().toULongLong(&userValid);
    record->userGeneration = user;
    if (!userValid || user != entry->session->inputArbiter()->userInputGeneration()) {
        _operations.finish(record->id, Error{ErrorCode::ControlInvalid}); return;
    }
    record->source = watch(entry->session);
    if (!entry->session->requestTerminalClear(record->id, request.all, request.expectedGeneration, record->cancelled))
        _operations.finish(record->id, Error{ErrorCode::Busy, "Clear command was not accepted by the Parser queue."});
}
void TerminalClearAdapter::finished(quint64 operation, quint64 generation, int status, quint64 nextLine)
{
    if (status != 0) {
        _operations.finish(operation, Error{status == 1 ? ErrorCode::Cancelled : ErrorCode::CursorStale}); return;
    }
    const auto records = _operations.records();
    for (const auto& record : records) if (record->id == operation && !record->result) {
        LogPage baseline;
        baseline.generation = generation;
        baseline.next.line = baseline.earliest.line = baseline.sampledEnd.line = nextLine;
        baseline.caughtUp = true;
        ClearCompletion result{generation, std::move(baseline), QDateTime::currentMSecsSinceEpoch(), clearTicketClock(), record->userGeneration};
        _operations.finish(operation, std::move(result)); return;
    }
}
}
