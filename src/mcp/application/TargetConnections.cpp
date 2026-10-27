#include "TargetConnections.h"
#include <QUuid>
#include <QScopeGuard>

namespace NovaTerm::Mcp {
void TargetConnections::setPorts(Open open, Query query, Close close, Created created)
{
    _open = std::move(open); _query = std::move(query); _close = std::move(close);
    _created = std::move(created);
}
TargetConnectionResult TargetConnections::call(const QString& client, quint64 version,
    const TargetConnectionRequest& request, Guard invocationValid)
{
    const auto failure = [](ErrorCode error) { return TargetConnectionResult{{}, error}; };
    const auto uuid = [](const QString& value) {
        return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value;
    };
    if (!uuid(request.targetId) || !uuid(request.operationId) || !uuid(request.configVersion))
        return failure(ErrorCode::InvalidArgument);
    if (request.action == TargetConnectionAction::Close && (request.epoch.isEmpty() || request.epoch.size() > 128))
        return failure(ErrorCode::InvalidArgument);
    const auto source = _targets.openingSource(client, version, request.targetId, request.configVersion);
    if (!source) return failure(ErrorCode::TargetOpenPermissionRequired);
    const Guard allowed = [this, client, version, request, invocationValid] {
        return (!invocationValid || invocationValid())
            && _targets.openingSource(client, version, request.targetId, request.configVersion).has_value();
    };
    if (!allowed()) return failure(ErrorCode::Cancelled);
    const auto clientRecords = _records.constFind(client);
    if (clientRecords != _records.cend()) {
        const auto previous = clientRecords->constFind(request.operationId);
        if (previous != clientRecords->cend()) {
            const auto record = *previous;
            if (record.identityVersion != version || record.targetId != request.targetId
                || record.configVersion != request.configVersion) return failure(ErrorCode::TargetOperationConflict);
            if (request.action == TargetConnectionAction::Close) {
                if (!record.facts.created || record.facts.sessionId.isEmpty())
                    return failure(ErrorCode::TargetCloseNotAllowed);
                if (record.closeAccepted) return request.epoch == record.closeEpoch
                    ? TargetConnectionResult{record.closeFacts, {}} : failure(ErrorCode::StaleSessionEpoch);
                if (record.closeInFlight) return failure(ErrorCode::TargetCloseBusy);
                _records[client][request.operationId].closeInFlight = true;
                const auto reset = qScopeGuard([this, client, operation = request.operationId] {
                    _records[client][operation].closeInFlight = false;
                });
                const auto query = _query;
                const auto current = query ? query(record.facts.sessionId, record.facts.attachmentId) : record.facts;
                if (current.state == "invalidated") return failure(ErrorCode::TargetCloseNotAllowed);
                if (request.epoch != current.epoch) return failure(ErrorCode::StaleSessionEpoch);
                if (!allowed()) return failure(ErrorCode::TargetOpenPermissionRequired);
                const auto close = _close;
                if (!close) return failure(ErrorCode::TargetCloseNotAllowed);
                const auto result = close(record.facts, request.epoch, allowed);
                auto& stored = _records[client][request.operationId];
                if (!result.error && (result.facts.state == "closing" || result.facts.state == "closed")) {
                    stored.closeAccepted = true; stored.closeFacts = result.facts;
                    stored.closeFacts.created = true;
                    stored.closeEpoch = request.epoch;
                }
                if (!allowed()) return failure(ErrorCode::TargetOpenPermissionRequired);
                return result;
            }
            auto facts = record.facts;
            const auto query = _query;
            if (!facts.sessionId.isEmpty() && query) {
                facts = query(facts.sessionId, facts.attachmentId);
                facts.created = record.facts.created;
            }
            if (!allowed())
                return failure(ErrorCode::TargetOpenPermissionRequired);
            return {facts, {}};
        }
    }
    if (request.action != TargetConnectionAction::Open) return failure(ErrorCode::TargetOperationNotAvailable);
    qsizetype count = 0;
    for (const auto& records : std::as_const(_records)) count += records.size();
    if (count >= 256 || (clientRecords != _records.cend() && clientRecords->size() >= 32))
        return failure(ErrorCode::TargetOperationLimit);
    _records[client].insert(request.operationId,
        {version, request.targetId, request.configVersion, {{}, {}, {}, "opening", {}, false}});
    // Copy the callable: composition or authorization callbacks may re-enter this index.
    const auto open = _open;
    const auto created = _created;
    const auto sharing = _targets.newSessionSharing(client, version, request.targetId);
    const auto facts = open ? open(*source, allowed)
        : TargetConnectionFacts{{}, {}, {}, "unavailable", "connection_ui_unavailable", false};
    _records[client][request.operationId].facts = facts;
    if (!allowed())
        return failure(ErrorCode::TargetOpenPermissionRequired);
    if (created && facts.created && !facts.sessionId.isEmpty())
        created(client, version, request, facts, sharing);
    return {facts, {}};
}
}
