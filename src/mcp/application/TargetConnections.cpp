#include "TargetConnections.h"
#include <QUuid>

namespace NovaTerm::Mcp {
void TargetConnections::setPorts(Open open, Query query)
{
    _open = std::move(open); _query = std::move(query);
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
    if (request.query) return failure(ErrorCode::TargetOperationNotAvailable);
    qsizetype count = 0;
    for (const auto& records : std::as_const(_records)) count += records.size();
    if (count >= 256 || (clientRecords != _records.cend() && clientRecords->size() >= 32))
        return failure(ErrorCode::TargetOperationLimit);
    _records[client].insert(request.operationId,
        {version, request.targetId, request.configVersion, {{}, {}, {}, "opening", {}, false}});
    // Copy the callable: composition or authorization callbacks may re-enter this index.
    const auto open = _open;
    const auto facts = open ? open(*source, allowed)
        : TargetConnectionFacts{{}, {}, {}, "unavailable", "connection_ui_unavailable", false};
    _records[client][request.operationId].facts = facts;
    if (!allowed())
        return failure(ErrorCode::TargetOpenPermissionRequired);
    return {facts, {}};
}
}
