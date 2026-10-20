#include "CapabilityAuthorization.h"
namespace NovaTerm::Capabilities {
struct CapabilityAuthorization::State { Lookup lookup; bool alive{true}; };
struct TrustedPrincipal::Seal {
    PrincipalId principal;
    EntryKind kind{EntryKind::Mcp};
    std::weak_ptr<void> authority;
    bool active{true};
};
void TrustedPrincipal::close() const { if (_seal) _seal->active = false; }
CapabilityAuthorization::CapabilityAuthorization(Lookup lookup)
    : _state(std::make_shared<State>(State{std::move(lookup), true})) {}
CapabilityAuthorization::~CapabilityAuthorization() { _state->alive = false; }
TrustedPrincipal CapabilityAuthorization::trust(PrincipalId id, EntryKind kind) const
{
    TrustedPrincipal trusted;
    trusted._seal = std::make_shared<TrustedPrincipal::Seal>(
        TrustedPrincipal::Seal{std::move(id), kind, _state, true});
    return trusted;
}
InvocationContext CapabilityAuthorization::context(const TrustedPrincipal& principal,
    ResourceBinding binding, quint64 version, OperationId operation, TimePoint deadline) const
{
    InvocationContext context;
    context._trusted = principal;
    if (principal._seal) context.principal = principal._seal->principal;
    context.resource = std::move(binding);
    context.accessVersion = version;
    context.operation = std::move(operation);
    context.deadline = deadline;
    return context;
}
std::optional<Rejection> CapabilityAuthorization::validate(const std::shared_ptr<State>& state,
    const InvocationContext& context, const Definition& definition)
{
    const auto& seal = context._trusted._seal;
    if (!state->alive || !seal || !seal->active || seal->principal.empty()
        || seal->principal != context.principal || seal->authority.lock() != state)
        return Rejection::UntrustedPrincipal;
    if (resourceKind(context.resource) != definition.resource) return Rejection::ResourceMismatch;
    if (!state->lookup) return Rejection::Denied;
    const auto snapshot = state->lookup(context.principal, seal->kind, context.resource, definition);
    if (!state->alive || !seal->active) return Rejection::UntrustedPrincipal;
    if (!snapshot) return Rejection::Denied;
    if (!sameResource(snapshot->resource, context.resource)) return Rejection::ResourceMismatch;
    if (snapshot->accessVersion != context.accessVersion) return Rejection::AuthorizationChanged;
    return std::nullopt;
}
std::optional<Rejection> CapabilityAuthorization::validate(const InvocationContext& context,
    const Definition& definition) const { const auto state = _state; return validate(state, context, definition); }
std::function<std::optional<Rejection>()> CapabilityAuthorization::validator(
    InvocationContext context, Definition definition) const
{
    return [state = _state, context = std::move(context), definition = std::move(definition)] {
        return validate(state, context, definition);
    };
}
}
