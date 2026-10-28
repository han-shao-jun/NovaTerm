#include "ReadCapabilities.h"
namespace NovaTerm::Capabilities::Terminal {
namespace {
bool matches(const InvocationLease& lease, const Mcp::SessionReference& reference)
{
    const auto* binding = std::get_if<SessionBinding>(&lease.context().resource);
    return binding && binding->session.value() == reference.session.str()
        && binding->epoch.value() == reference.epoch.str();
}
}
ReadCapabilities::ReadCapabilities(CapabilityDispatcher& dispatcher, const Mcp::ReadSessionPort& port)
    : sessions(dispatcher, CapabilityId{"session.list"},
        [&port](SessionListQuery request, InvocationLease lease, auto done) {
            done(Mcp::ReadUseCases{port}.listSessions(Mcp::ClientId{lease.context().principal.value()},
                request.limit, request.cursor, request.humanConfirmation));
        })
    , context(dispatcher, CapabilityId{"terminal.context.read"},
        [&port](ContextReadQuery request, InvocationLease lease, auto done) {
            if (!matches(lease, request.query.session)) { done(Rejection::ResourceMismatch); return; }
            if (request.query.maxBytes < 1 || request.query.maxBytes > Mcp::Budget::MaxTextBytes
                || request.query.maxLines < 1 || request.query.maxLines > 1024) {
                done(Mcp::Result<Mcp::ContextPage>{Mcp::Error{Mcp::ErrorCode::InvalidArgument}}); return;
            }
            done(Mcp::ReadUseCases{port}.readContext(Mcp::ClientId{lease.context().principal.value()},
                std::move(request.query), request.cursor));
        })
{}
}
