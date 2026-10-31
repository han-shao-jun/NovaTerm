#include "InputCapabilities.h"
#include "mcp/contracts/TerminalInputValidation.h"
#include <QScopeGuard>
namespace NovaTerm::Capabilities::Terminal {
namespace {
bool matches(const InvocationLease& lease, const Mcp::SessionReference& reference)
{
    const auto* binding = std::get_if<SessionBinding>(&lease.context().resource);
    return binding && binding->session.value() == reference.session.str() && binding->epoch.value() == reference.epoch.str();
}
}
InputCapabilities::InputCapabilities(CapabilityDispatcher& dispatcher, Mcp::ObservationState& state,
    TerminalInputPort& port, std::function<bool()> ownerAlive)
    : input(dispatcher, CapabilityId{"terminal.interact"},
        [&state, &port, ownerAlive](InputSubmission call, InvocationLease lease, auto done) {
            const auto alive = [&ownerAlive] { return !ownerAlive || ownerAlive(); };
            if (!alive()) { done(Rejection::Unavailable); return; }
            const Mcp::SessionReference reference{call.request.session, call.request.epoch};
            if (!matches(lease, reference)) { done(Rejection::ResourceMismatch); return; }
            const auto invalid = Mcp::validateTerminalActions(call.request.actions);
            if (!invalid.isEmpty()) { done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::InvalidArgument, invalid}}); return; }
            const Mcp::ClientId client{lease.context().principal.value()};
            const auto facts = port.facts(client, reference);
            const auto prepared = state.prepareInput(client, call.request, call.claims, facts);
            if (const auto* error = Mcp::errorIfAny(prepared)) { done(Mcp::Result<Mcp::InputAdmission>{*error}); return; }
            const auto admission = std::get<Mcp::InputAdmission>(prepared);
            if (admission == Mcp::InputAdmission::Replay && !call.allowReplay) {
                done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::InputSequenceConflict,
                    "A new task cannot attach to an existing terminal input operation."}}); return;
            }
            if (admission != Mcp::InputAdmission::Replay) {
                auto reserved = state.reserveInput(client, call.request, facts);
                if (const auto* error = Mcp::errorIfAny(reserved)) { done(Mcp::Result<Mcp::InputAdmission>{*error}); return; }
                auto finish = std::move(std::get<Mcp::ObservationState::InputReservation>(reserved));
                const auto rollback = qScopeGuard([&finish] { finish(false); });
                if (const auto reason = lease.check()) { done(*reason); return; }
                if (admission == Mcp::InputAdmission::Release) {
                    port.release(client, call.request);
                    finish(true);
                }
                else {
                    const bool acquired = port.acquire(client, reference);
                    if (!alive()) { done(Rejection::Unavailable); return; }
                    if (!acquired) {
                        done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::TerminalBusy, "Terminal input is owned by another operation."}}); return;
                    }
                    if (const auto reason = lease.check()) { port.release(client, call.request); done(*reason); return; }
                    const bool submitted = port.submit(client, call.request);
                    if (submitted) finish(true);
                    if (!alive()) { done(Rejection::Unavailable); return; }
                    if (!submitted) {
                        done(Mcp::Result<Mcp::InputAdmission>{Mcp::Error{Mcp::ErrorCode::InputBusy, "Input was not accepted."}}); return;
                    }
                }
                if (!alive()) { done(Rejection::Unavailable); return; }
                const bool retained = finish(true);
                const auto after = port.facts(client, reference);
                if (retained && !after.unavailable && after.session.session == facts.session.session
                    && after.session.epoch == facts.session.epoch && after.grant == facts.grant
                    && after.cacheGeneration == facts.cacheGeneration && after.user == facts.user)
                    state.inputAccepted(client, call.request, after);
                if (admission == Mcp::InputAdmission::Submit && call.submitted) call.submitted();
            }
            done(prepared);
        })
    , elevation(dispatcher, CapabilityId{"terminal.elevation.request"},
        [&state, &port, ownerAlive](ElevationSubmission call, InvocationLease lease, auto done) {
            if (ownerAlive && !ownerAlive()) { done(Rejection::Unavailable); return; }
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
