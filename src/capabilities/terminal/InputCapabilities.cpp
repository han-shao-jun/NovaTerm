#include "InputCapabilities.h"
#include "mcp/contracts/TerminalInputValidation.h"
namespace NovaTerm::Capabilities::Terminal {
namespace {
bool matches(const InvocationLease& lease, const Mcp::SessionReference& reference)
{
    const auto* binding = std::get_if<SessionBinding>(&lease.context().resource);
    return binding && binding->session.value() == reference.session.str() && binding->epoch.value() == reference.epoch.str();
}
}
InputCapabilities::InputCapabilities(CapabilityDispatcher& dispatcher, Mcp::ObservationState& state, TerminalInputPort& port)
    : input(dispatcher, CapabilityId{"terminal.interact"},
        [&state, &port](InputSubmission call, InvocationLease lease, auto done) {
            const Mcp::SessionReference reference{call.request.session, call.request.epoch};
            if (!matches(lease, reference)) { done(Rejection::ResourceMismatch); return; }
            const auto invalid = Mcp::validateTerminalActions(call.request.actions);
            if (!invalid.isEmpty()) { done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::InvalidArgument, invalid}}); return; }
            const Mcp::ClientId client{lease.context().principal.value()};
            const auto facts = port.facts(client, reference);
            const auto prepared = state.prepareInput(client, call.request, call.claims, facts);
            if (const auto* error = Mcp::errorIfAny(prepared)) { done(Mcp::Result<Mcp::InputAdmission>{*error}); return; }
            const auto admission = std::get<Mcp::InputAdmission>(prepared);
            if (admission != Mcp::InputAdmission::Replay) {
                if (const auto reason = lease.check()) { done(*reason); return; }
                if (admission == Mcp::InputAdmission::Release) port.release(client, call.request);
                else {
                    if (!port.acquire(client, reference)) {
                        done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::TerminalBusy, "Terminal input is owned by another operation."}}); return;
                    }
                    if (const auto reason = lease.check()) { port.release(client, call.request); done(*reason); return; }
                    if (!port.submit(client, call.request)) {
                        done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::InputBusy, "Input was not accepted."}}); return;
                    }
                }
                state.inputAccepted(client, call.request, port.facts(client, reference));
            }
            done(prepared);
        })
    , elevation(dispatcher, CapabilityId{"terminal.elevation.request"},
        [&state, &port](ElevationSubmission call, InvocationLease lease, auto done) {
            const Mcp::SessionReference reference{call.request.session, call.request.epoch};
            if (!matches(lease, reference)) { done(Rejection::ResourceMismatch); return; }
            const Mcp::ClientId client{lease.context().principal.value()};
            Mcp::Result<std::monostate> result{Mcp::Error{Mcp::ErrorCode::ElevationAdapterUnavailable, "No trusted authentication adapter is installed for this connection."}};
            const auto facts = port.facts(client, reference);
            if (!port.canElevate(client, reference)) result = Mcp::Error{Mcp::ErrorCode::ElevationPermissionRequired, "Managed elevation is not authorized."};
            else if (const auto error = state.validateInput(call.claims, client, facts)) result = *error;
            done(result);
        })
{}
}
