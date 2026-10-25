#include "LogReadAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "tasks/LogReadUseCase.h"
#include "session/SessionDirectory.h"
#include <QDateTime>
#include <QJsonArray>

namespace NovaTerm::Mcp {
namespace {
class SessionLogPort final : public LogReadPort {
public:
    SessionLogPort(AccessStore& access, SessionDirectory& sessions) : _access(access), _sessions(sessions) {}
    bool authorized(const ClientId& client, const SessionReference& session) const override {
        const auto entry = _sessions.find(session.session.str());
        return entry && entry->epoch == session.epoch.str() && entry->session
            && _access.canRead(client.str(), *entry);
    }
    ClientAccessVersion version(const ClientId& client) const override { return ClientAccessVersion{_access.version(client.str())}; }
    Result<LogPage> read(const SessionReference& session, const LogRequest& request) const override {
        const auto entry = _sessions.find(session.session.str());
        if (!entry || !entry->session || entry->epoch != session.epoch.str() || !entry->session->core())
            return Error{ErrorCode::SessionNotAvailable};
        const auto page = entry->session->core()->tryReadLog(request);
        if (!page) return Error{ErrorCode::Busy, "Terminal model is busy; retry the same position.", RetryHint{true, std::chrono::milliseconds(50)}};
        return *page;
    }
private:
    AccessStore& _access;
    SessionDirectory& _sessions;
};
QJsonObject position(const LogPosition& value) {
    return {{"lineId", QString::number(value.line)}, {"cellOffset", QString::number(value.cell)},
        {"scalarOffset", int(value.scalar)}, {"byteOffset", QString::number(value.byte)}};
}
const char* statusName(LogStatus status) {
    switch (status) {
    case LogStatus::Ok: return "ok";
    case LogStatus::Gap: return "gap";
    case LogStatus::Invalidated: return "invalidated";
    case LogStatus::Pending: return "pending";
    case LogStatus::Busy: return "busy";
    case LogStatus::Unavailable: return "unavailable";
    case LogStatus::BudgetTooSmall: case LogStatus::InvalidPosition: break;
    }
    return "unavailable";
}
}

Result<QJsonObject> readLogContext(AccessStore& access, SessionDirectory& sessions,
    const QString& instance, const QString& client, const QByteArray& key, const ReadContextRequest& typed)
{
    // Defense for typed callers as well as wire validation.
    if (typed.source != ContextSource::Log || typed.start.has_value() == typed.sinceToken.has_value()
        || typed.expandToken || typed.waitFor || (typed.wait && typed.wait->count() != 0)
        || typed.includeViewport.value_or(false)) return Error{ErrorCode::InvalidArgument};
    SessionLogPort port{access, sessions};
    const SessionReference session{typed.session, typed.epoch};
    const ClientId principal{client};
    const auto grant = port.version(principal);
    LogRequest request;
    request.start = typed.start == LogInitialPosition::Tail ? LogStart::Tail : LogStart::Oldest;
    const bool compact = typed.projection == ProjectionMode::Compact;
    request.maxBytes = size_t(typed.maxBytes.value_or(compact ? Budget::CompactPayloadBytes : Budget::ObservationDefaultPayloadBytes));
    request.maxLines = size_t(typed.maxLines.value_or(compact ? Budget::CompactLines : Budget::ObservationDefaultLines));
    std::optional<ClientAccessVersion> cursorGrant;
    if (typed.sinceToken) {
        const auto token = verifyToken(*typed.sinceToken, key);
        if (!token || token->value("kind") != "log" || token->value("version") != 1
            || token->value("client") != client || token->value("session") != typed.session.str()
            || token->value("epoch") != typed.epoch.str()) return Error{ErrorCode::CursorStale};
        bool valid = true;
        const auto count = [&token, &valid](const char* name) {
            bool ok = false;
            const auto value = token->value(name).toString().toULongLong(&ok);
            valid &= ok; return value;
        };
        LogCursor cursor;
        cursor.generation = count("generation"); cursor.position.line = count("line");
        cursor.position.cell = count("cell"); cursor.position.byte = count("byte");
        const auto scalar = count("scalar");
        cursorGrant = ClientAccessVersion{count("grant")};
        if (!valid || scalar >= MaxCharsPerCell) return Error{ErrorCode::CursorStale};
        cursor.position.scalar = unsigned(scalar); request.cursor = cursor;
    }
    const auto result = readLog(port, principal, session, request, cursorGrant);
    if (const auto* error = errorIfAny(result)) return *error;
    const auto& page = std::get<LogPage>(result);
    if (page.status == LogStatus::BudgetTooSmall) return Error{ErrorCode::TextBudgetTooSmall};
    if (page.status == LogStatus::InvalidPosition) return Error{ErrorCode::CursorStale};
    const auto encode = [&](const LogPosition& value, u64 generation) {
        return signToken({{"kind", "log"}, {"version", 1}, {"client", client},
            {"session", typed.session.str()}, {"epoch", typed.epoch.str()},
            {"grant", QString::number(grant.value())}, {"generation", QString::number(generation)},
            {"line", QString::number(value.line)}, {"cell", QString::number(value.cell)},
            {"scalar", QString::number(value.scalar)}, {"byte", QString::number(value.byte)}}, key);
    };
    QJsonArray fragments;
    for (const auto& part : page.fragments) fragments.append(QJsonObject{{"lineId", QString::number(part.line)},
        {"byteOffset", QString::number(part.byteOffset)}, {"text", QString::fromUtf8(part.text)},
        {"lineComplete", part.lineComplete}, {"hardBreak", part.hardBreak}});
    // Invalidated cursors retain their old generation; never launder them into a new baseline.
    const auto nextGeneration = page.status == LogStatus::Invalidated && request.cursor
        ? request.cursor->generation : page.generation;
    return QJsonObject{{"instanceId", instance}, {"sessionId", typed.session.str()}, {"epoch", typed.epoch.str()},
        {"source", "log"}, {"coverage", "committed_history"}, {"historyMayPrecedeEpoch", true},
        {"capturedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"cacheGeneration", QString::number(page.generation)}, {"status", statusName(page.status)}, {"fragments", fragments},
        {"nextPosition", position(page.next)}, {"earliestPosition", position(page.earliest)}, {"sampledEndPosition", position(page.sampledEnd)},
        {"nextToken", encode(page.next, nextGeneration)}, {"earliestToken", encode(page.earliest, page.generation)},
        {"sampledEndToken", encode(page.sampledEnd, page.generation)}, {"hasMore", page.hasMore}, {"caughtUp", page.caughtUp}};
}
} // namespace NovaTerm::Mcp
