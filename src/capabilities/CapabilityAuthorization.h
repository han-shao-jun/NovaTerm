#pragma once
#include "CapabilityTypes.h"
#include "CapabilityResult.h"
#include <functional>
#include <memory>

namespace NovaTerm::Capabilities {
struct AuthorizationSnapshot {
    ResourceBinding resource;
    quint64 accessVersion{0};
};
class CapabilityAuthorization;
class TrustedPrincipal
{
public:
    TrustedPrincipal() = default;
    void close() const;
private:
    struct Seal;
    std::shared_ptr<Seal> _seal;
    friend class CapabilityAuthorization;
};
struct InvocationContext {
    PrincipalId principal;
    ResourceBinding resource;
    quint64 accessVersion{0};
    TimePoint deadline;
    OperationId operation;
private:
    TrustedPrincipal _trusted;
    friend class CapabilityAuthorization;
};
// All methods and leases are used on the owning application thread.
// Trust/context are composition APIs, never model parameters or wire codecs.
class CapabilityAuthorization
{
public:
    using Lookup = std::function<std::optional<AuthorizationSnapshot>(
        const PrincipalId&, EntryKind, const ResourceBinding&, const Definition&)>;
    explicit CapabilityAuthorization(Lookup lookup);
    ~CapabilityAuthorization();
    CapabilityAuthorization(const CapabilityAuthorization&) = delete;
    CapabilityAuthorization& operator=(const CapabilityAuthorization&) = delete;
    TrustedPrincipal trust(PrincipalId, EntryKind) const;
    InvocationContext context(const TrustedPrincipal&, ResourceBinding, quint64 accessVersion,
        OperationId, TimePoint deadline) const;
    std::optional<Rejection> validate(const InvocationContext&, const Definition&) const;
    // Retained by leases; destruction of the authority makes it fail closed.
    std::function<std::optional<Rejection>()> validator(InvocationContext, Definition) const;
private:
    struct State;
    std::shared_ptr<State> _state;
    static std::optional<Rejection> validate(const std::shared_ptr<State>&,
        const InvocationContext&, const Definition&);
};
}
