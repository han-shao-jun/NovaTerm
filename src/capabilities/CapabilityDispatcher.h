#pragma once
#include "CapabilityAuthorization.h"
#include "CapabilityRegistry.h"
#include <functional>
#include <memory>
namespace NovaTerm::Capabilities {
class CapabilityDispatcher;
class InvocationLease
{
public:
    InvocationLease() = default;
    ~InvocationLease() = default;
    InvocationLease(InvocationLease&&) noexcept = default;
    InvocationLease& operator=(InvocationLease&&) noexcept = default;
    InvocationLease(const InvocationLease&) = delete;
    InvocationLease& operator=(const InvocationLease&) = delete;
    std::optional<Rejection> check() const;
    // Authorization remains independently checkable after cancellation or expiry.
    std::optional<Rejection> checkAuthorization() const;
    const InvocationContext& context() const;
    CapabilityId capability() const;
    bool finish();
    void onCancel(std::function<void()>);
    void setDeadline(TimePoint);
    // Exactly-once, authorization-checked delivery. It retains only admission state.
    std::function<std::optional<Rejection>()> completion() const;
    std::function<std::optional<Rejection>()> validation() const;
private:
    struct State;
    std::shared_ptr<State> _state;
    explicit InvocationLease(std::shared_ptr<State> state) : _state(std::move(state)) {}
    friend class CapabilityDispatcher;
};
class CapabilityDispatcher
{
public:
    using Now = std::function<TimePoint()>;
    CapabilityDispatcher(const CapabilityRegistry&, const CapabilityAuthorization&, Now now = Clock::now);
    ~CapabilityDispatcher();
    CapabilityDispatcher(const CapabilityDispatcher&) = delete;
    CapabilityDispatcher& operator=(const CapabilityDispatcher&) = delete;
    Result<InvocationLease> admit(const CapabilityId&, const InvocationContext&);
    bool cancel(const PrincipalId&, const OperationId&);
    void stop();
    size_t activeCount() const;
    static constexpr size_t MaxOperations = 128;
private:
    struct State;
    std::shared_ptr<State> _state;
    const CapabilityRegistry& _registry;
    const CapabilityAuthorization& _authorization;
};
}
