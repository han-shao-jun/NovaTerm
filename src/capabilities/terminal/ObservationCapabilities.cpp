#include "ObservationCapabilities.h"
#include "LegacyTerminalMapping.h"
#include <map>
namespace NovaTerm::Capabilities::Terminal {
struct ObservationCapabilities::State : std::enable_shared_from_this<State> {
    struct Active { InvocationLease lease; Completion<Mcp::ObservationCompletion> done; };
    Mcp::ObservationState& observation;
    std::map<QString, Active> active;
    bool closed{false};
    explicit State(Mcp::ObservationState& value) : observation(value) {}
    void close() {
        closed = true;
        auto owned = std::move(active);
        for (const auto& item : owned) observation.cancel(Mcp::RequestId{item.first});
    }
    void complete(Mcp::ObservationCompletion completion) {
        auto node = active.extract(completion.wait.request.str());
        if (node.empty()) return;
        auto owned = std::move(node.mapped());
        owned.done(std::move(completion));
    }
    void cancel(const Mcp::RequestId& id) {
        observation.cancel(id);
        active.erase(id.str());
    }
    void begin(Mcp::ObservationWait wait, InvocationLease lease, Completion<Mcp::ObservationCompletion> done) {
        const auto& context = lease.context();
        const auto* binding = std::get_if<SessionBinding>(&context.resource);
        if (closed || !binding || binding->session.value() != wait.session.session.str()
            || binding->epoch.value() != wait.session.epoch.str()
            || Mcp::terminalCapability(wait.typed) != lease.capability()) {
            done(Rejection::ResourceMismatch); return;
        }
        if (wait.request.isEmpty() || active.count(wait.request.str())) { done(Rejection::DuplicateOperation); return; }
        wait.client = Mcp::ClientId{context.principal.value()};
        wait.grant = Mcp::ClientAccessVersion{context.accessVersion};
        const auto admitted = observation.admit(wait);
        if (const auto* error = Mcp::errorIfAny(admitted)) {
            done(Mcp::ObservationCompletion{wait, *error, Mcp::ObservationWake::Deadline}); return;
        }
        const auto id = wait.request;
        const std::weak_ptr<State> weak = weak_from_this();
        lease.onCancel([weak, id] { if (const auto owner = weak.lock(); owner && !owner->closed) owner->cancel(id); });
        active.emplace(id.str(), Active{std::move(lease), std::move(done)});
        for (const auto& replaced : std::get<QList<Mcp::ObservationCompletion>>(admitted)) {
            if (closed) break;
            complete(replaced);
        }
    }
};
ObservationCapabilities::ObservationCapabilities(CapabilityDispatcher& dispatcher, Mcp::ObservationState& state)
    : _state(std::make_shared<State>(state))
    , read(dispatcher, CapabilityId{"terminal.context.read"}, [owner = _state](auto wait, InvocationLease lease, auto done) {
        owner->begin(std::move(wait), std::move(lease), std::move(done));
    })
    , input(dispatcher, CapabilityId{"terminal.interact"}, [owner = _state](auto wait, InvocationLease lease, auto done) {
        owner->begin(std::move(wait), std::move(lease), std::move(done));
    })
    , elevation(dispatcher, CapabilityId{"terminal.elevation.request"}, [owner = _state](auto wait, InvocationLease lease, auto done) {
        owner->begin(std::move(wait), std::move(lease), std::move(done));
    })
{}
ObservationCapabilities::~ObservationCapabilities() { _state->close(); }
void ObservationCapabilities::complete(Mcp::ObservationCompletion completion) {
    const auto state = _state; state->complete(std::move(completion));
}
void ObservationCapabilities::cancel(const Mcp::RequestId& id) { _state->cancel(id); }
}
