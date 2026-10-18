/** @file ReadUseCases.cpp @brief 只读工具用例实现。 */
#include "ReadUseCases.h"

#include "mcp/CapabilityAdapter.h"
#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include "mcp/contracts/Budget.h"
#include "mcp/ObservationService.h"
#include "mcp/TerminalInteraction.h"
#include "mcp/CommandPolicy.h"
#include "session/SessionCommandFacade.h"

#include <QJsonArray>
#include <QJsonValue>

namespace NovaTerm::Mcp {

ReadUseCases::ReadUseCases(Ports ports) : _ports(std::move(ports)) {}

Result<QJsonObject> ReadUseCases::listSessions(const QString& client,
    const QByteArray& key, bool formElicitation, const ListSessionsRequest& request) const
{
    const auto* access = _ports.access;
    auto* sessions = _ports.sessions;
    if (!access || !sessions || !_ports.terminal) {
        return Error{ErrorCode::SessionNotAvailable, QStringLiteral("SESSION_NOT_AVAILABLE")};
    }
    QString after;
    const auto cursor = request.cursor.value_or(QString{});
    if (!cursor.isEmpty()) {
        const auto token = verifyToken(cursor, key);
        if (!token || token->value("kind") != "directory"
            || token->value("version").toString() != QString::number(sessions->revision())
            || token->value("grant").toString()
                != QString::number(access->version(client))) {
            return Error{ErrorCode::CursorStale, QStringLiteral("CURSOR_STALE")};
        }
        after = token->value("after").toString();
    }
    const int limit = request.limit;
    const auto versionBeforePage = sessions->revision();
    const auto all = sessions->pageAfter(after, limit + 1, [&](const SessionDirectory::Entry& entry) {
        return access->canRead(client, entry) && entry.state != SessionState::Closing
            && entry.state != SessionState::Closed;
    });
    if (!cursor.isEmpty() && versionBeforePage != sessions->revision())
        return Error{ErrorCode::CursorStale, QStringLiteral("CURSOR_STALE")};
    QJsonArray rows;
    QString last;
    bool more = false;
    for (const auto& entry : all) {
        if (entry.id <= after || !access->canRead(client, entry)
            || entry.state == SessionState::Closing || entry.state == SessionState::Closed)
            continue;
        if (rows.size() >= limit) { more = true; break; }
        bool truncated = false;
        const auto title = boundedText(entry.title, 256, &truncated);
        const auto metadata = _ports.terminal->capabilities(client, entry.id,
            formElicitation);
        QJsonObject row{{"sessionId", entry.id}, {"epoch", entry.epoch},
            {"state", SessionDirectory::stateName(entry.state)},
            {"transport", SessionDirectory::transportName(entry.kind)},
            {"displayName", title}, {"displayNameTruncated", truncated},
            {"capabilities", QJsonArray{}}};
        for (auto it = metadata.begin(); it != metadata.end(); ++it)
            row.insert(it.key(), it.value());
        rows.append(row);
        last = entry.id;
    }
    QJsonValue next(QJsonValue::Null);
    if (more) next = signToken({{"kind", "directory"}, {"after", last},
        {"version", QString::number(sessions->revision())},
        {"grant", QString::number(access->version(client))}}, key);
    return QJsonObject{{"instanceId", _ports.instance},
        {"applicationVersion", NOVATERM_VERSION}, {"sessions", rows},
        {"nextCursor", next}};
}

Result<QJsonObject> ReadUseCases::readContext(const SessionDirectory::Entry& entry,
    const QString& owner, const QString& client, const QByteArray& key,
    const ReadContextRequest& request) const
{
    auto* access = _ports.access;
    if (!access || !_ports.observation) {
        return Error{ErrorCode::SessionNotAvailable, QStringLiteral("SESSION_NOT_AVAILABLE")};
    }
    quint64 since = 0;
    const bool viewport = request.includeViewport.value_or(true);
    const auto encoded = request.sinceToken.value_or(QString());
    if (!encoded.isEmpty()) {
        const auto token = verifyToken(encoded, key);
        bool ok = false;
        if (token) since = token->value("revision").toString().toULongLong(&ok);
        if (!token || !ok || token->value("kind") != "context"
            || token->value("session") != entry.id
            || token->value("epoch") != entry.epoch
            || token->value("viewport").toBool() != viewport
            || token->value("grant").toString() != QString::number(access->version(client))) {
            return Error{ErrorCode::CursorStale, QStringLiteral("CURSOR_STALE")};
        }
    }
    ObservationReadRequest read;
    read.sessionId = entry.id;
    read.epoch = entry.epoch;
    read.owner = owner;
    read.captureId = newId();
    read.includeViewport = viewport;
    read.sinceProvided = !encoded.isEmpty();
    read.sinceRevision = since;
    read.freshness = NovaTerm::publishedContextSnapshotEnabled()
        ? FreshnessPolicy::BoundedStaleness : FreshnessPolicy::StrictRevision;
    read.maxBytes = request.maxBytes.value_or(Budget::ObservationDefaultPayloadBytes);
    read.maxLines = request.maxLines.value_or(Budget::ObservationDefaultLines);
    const auto observed = _ports.observation->read(read, entry);
    if (observed.status == ObservationRead::Status::Busy)
        return Error{ErrorCode::Busy, QStringLiteral("BUSY"), RetryHint{true, std::chrono::milliseconds(250)}};
    if (observed.status == ObservationRead::Status::BusyShort)
        return Error{ErrorCode::Busy, QStringLiteral("BUSY"), RetryHint{true, std::chrono::milliseconds(50)}};
    QJsonValue next(QJsonValue::Null);
    if (!observed.sourceTruncated && !observed.outputTruncated) {
        next = signToken({{"kind", "context"}, {"session", entry.id},
            {"epoch", entry.epoch}, {"revision", observed.revision},
            {"grant", QString::number(access->version(client))},
            {"viewport", viewport}}, key);
    }
    QJsonArray screen, recent;
    for (const auto& line : observed.viewport)
        screen.append(QJsonObject{{"text", line.text}});
    for (const auto& line : observed.recent)
        recent.append(QJsonObject{{"text", line.text}});
    return QJsonObject{{"instanceId", _ports.instance}, {"sessionId", entry.id},
        {"epoch", entry.epoch}, {"revision", observed.revision},
        {"captureId", observed.captureId}, {"capturedAt", observed.capturedAt},
        {"state", SessionDirectory::stateName(entry.state)},
        {"transport", SessionDirectory::transportName(entry.kind)},
        {"title", observed.title}, {"alternateScreen", observed.alternateScreen},
        {"cursor", QJsonObject{{"row", observed.cursorRow},
            {"column", observed.cursorColumn}}},
        {"viewport", screen}, {"recentOutput", recent},
        {"coverage", "bounded_summary"},
        {"historyMayPrecedeEpoch", observed.historyMayPrecedeEpoch},
        {"sourceTruncated", observed.sourceTruncated},
        {"outputTruncated", observed.outputTruncated},
        {"resetRequired", observed.resetRequired},
        {"suppressedDuplicates", QString::number(observed.suppressedDuplicates)},
        {"nextToken", next}};
}

Result<QJsonObject> ReadUseCases::listCommands(const SessionDirectory::Entry& entry,
    const QString& client, const QByteArray& key, qint64 now) const
{
    auto* access = _ports.access;
    if (!access) {
        return Error{ErrorCode::SessionNotAvailable, QStringLiteral("SESSION_NOT_AVAILABLE")};
    }
    const auto allowed = access->commands(client, entry);
    QString reason;
    auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
    const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
    const QString policyVersion = CommandPolicy::version(profile);
    if (entry.state != SessionState::Running) reason = "SESSION_NOT_READY";
    else if (!profile.isAvailable()) reason = "COMMAND_PROFILE_UNAVAILABLE";
    else if (!facade || !facade->isAvailable()) reason = "COMMAND_UNAVAILABLE";
    else if (allowed.isEmpty()) reason = "COMMAND_PERMISSION_REQUIRED";
    else if (entry.targetFingerprint.isEmpty()) reason = "COMMAND_UNAVAILABLE";
    QJsonArray catalog;
    if (reason.isEmpty()) for (const auto& command : CommandPolicy::catalog(profile)) {
        if (!allowed.contains(command.id)) continue;
        const auto ticket = signToken({{"kind", "command"}, {"execution", newId()},
            {"session", entry.id}, {"epoch", entry.epoch}, {"command", command.id},
            {"policy", policyVersion}, {"profile", profile.version()},
            {"grant", QString::number(access->version(client))},
            {"expires", now + Budget::ConfirmationTimeout.count()}}, key);
        const QString preview = entry.kind == TransportKind::LocalShell
            && facade->capabilities().mode == CommandExecutionMode::Isolated
            ? QStringLiteral("novaterm-local-diag ") + command.id
            : QString::fromUtf8(command.command);
        catalog.append(QJsonObject{{"commandId", command.id}, {"title", command.title},
            {"preview", preview},
            {"argumentSchema", QJsonObject{{"type", "object"}, {"additionalProperties", false}}},
            {"timeoutMs", int(Budget::CommandTimeout.count())}, {"maxOutputBytes", CommandOutputBytes},
            {"commandTicket", ticket}});
    }
    return QJsonObject{{"instanceId", _ports.instance}, {"sessionId", entry.id},
        {"epoch", entry.epoch}, {"policyVersion", policyVersion},
        {"executionEnabled", reason.isEmpty()},
        {"disabledReason", reason.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(reason)},
        {"commands", catalog}};
}

} // namespace NovaTerm::Mcp
