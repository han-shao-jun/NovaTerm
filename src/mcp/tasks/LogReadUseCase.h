#pragma once
#include "core/scrollback/TerminalLog.h"
#include "mcp/contracts/Budget.h"
#include "mcp/contracts/Errors.h"
#include "mcp/contracts/SessionBinding.h"

namespace NovaTerm::Mcp {
// No mutable state, Session/HTTP/JSON or history pin ownership in this use case.
class LogReadPort {
public:
    virtual ~LogReadPort() = default;
    virtual bool authorized(const ClientId&, const SessionReference&) const = 0;
    virtual ClientAccessVersion version(const ClientId&) const = 0;
    virtual Result<LogPage> read(const SessionReference&, const LogRequest&) const = 0;
};
[[nodiscard]] inline Result<LogPage> readLog(const LogReadPort& port, const ClientId& client,
    const SessionReference& session, const LogRequest& request, std::optional<ClientAccessVersion> cursorGrant)
{
    if (!port.authorized(client, session)) return Error{ErrorCode::SessionNotAvailable};
    const auto grant = port.version(client);
    if (cursorGrant && *cursorGrant != grant) return Error{ErrorCode::CursorStale};
    if (!request.maxBytes || request.maxBytes > size_t(Budget::MaxTextBytes)
        || !request.maxLines || request.maxLines > size_t(Budget::MaxFullLines))
        return Error{ErrorCode::InvalidArgument};
    auto result = port.read(session, request);
    if (!port.authorized(client, session)) return Error{ErrorCode::SessionNotAvailable};
    if (port.version(client) != grant) return Error{ErrorCode::CursorStale};
    return result;
}
} // namespace NovaTerm::Mcp
