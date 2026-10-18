/** @file ReadUseCases.cpp @brief 分页绑定、上下文续读与诊断目录规则。 */
#include "ReadUseCases.h"

namespace NovaTerm::Mcp {

ReadUseCases::ReadUseCases(const ReadSessionPort& sessions) : _sessions(sessions) {}

Result<SessionPage> ReadUseCases::listSessions(const ClientId& client, int limit,
    const std::optional<DirectoryCursor>& cursor, bool humanConfirmation) const
{
    // 同样供非 MCP 入口调用，不能依赖 wire Schema 提供有效页大小。
    if (limit < 1 || limit > 200)
        return Error{ErrorCode::InvalidArgument};
    const auto revision = _sessions.directoryRevision();
    const auto grant = _sessions.accessVersion(client);
    if (cursor && (cursor->directoryRevision != revision || cursor->accessVersion != grant))
        return Error{ErrorCode::CursorStale};

    const auto after = cursor ? cursor->after : SessionId{};
    const auto rows = _sessions.pageAfter(client, after, limit + 1, humanConfirmation);
    if (revision != _sessions.directoryRevision() || grant != _sessions.accessVersion(client))
        return Error{ErrorCode::CursorStale};

    SessionPage page;
    for (const auto& row : rows) {
        if (!row.visible || row.reference.session.str() <= after.str())
            continue;
        if (page.sessions.size() == limit) {
            page.next = DirectoryCursor{page.sessions.last().reference.session, revision, grant};
            break;
        }
        page.sessions.append(row);
    }
    return page;
}

Result<ContextPage> ReadUseCases::readContext(const ClientId& client,
    ContextQuery query, const std::optional<ContextCursor>& cursor) const
{
    query.client = client;
    const auto grant = _sessions.accessVersion(client);
    if (cursor) {
        if (cursor->session.session != query.session.session
            || cursor->session.epoch != query.session.epoch
            || cursor->includeViewport != query.includeViewport
            || cursor->accessVersion != grant)
            return Error{ErrorCode::CursorStale};
        query.sinceRevision = cursor->revision;
    }
    const auto read = _sessions.readContext(query);
    if (const auto* error = errorIfAny(read))
        return *error;
    if (grant != _sessions.accessVersion(client))
        return Error{ErrorCode::CursorStale};
    ContextPage page{std::get<ContextSummary>(read), std::nullopt};
    if (!page.summary.sourceTruncated && !page.summary.outputTruncated)
        page.next = ContextCursor{query.session, page.summary.revision, grant, query.includeViewport};
    return page;
}

Result<CommandList> ReadUseCases::listCommands(const ClientId& client,
    const ReadSession& session, qint64 now) const
{
    const auto catalog = _sessions.commandCatalog(client, session.reference);
    CommandList result;
    result.session = session.reference;
    result.policyVersion = catalog.policyVersion;
    result.profileVersion = catalog.profileVersion;
    result.accessVersion = _sessions.accessVersion(client);
    result.expires = now + Budget::ConfirmationTimeout.count();
    if (!session.running) result.disabledReason = ErrorCode::SessionNotReady;
    else if (!catalog.profileAvailable) result.disabledReason = ErrorCode::CommandProfileUnavailable;
    else if (!catalog.executorAvailable) result.disabledReason = ErrorCode::CommandUnavailable;
    else if (catalog.allowed.isEmpty()) result.disabledReason = ErrorCode::CommandPermissionRequired;
    else if (!catalog.targetVerified) result.disabledReason = ErrorCode::CommandUnavailable;
    if (!result.disabledReason) {
        for (const auto& command : catalog.commands) {
            if (catalog.allowed.contains(command.id))
                result.commands.append(command);
        }
    }
    return result;
}

} // namespace NovaTerm::Mcp
