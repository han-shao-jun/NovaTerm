#include "CapabilityDispatcher.h"
#include <map>
#include <cassert>
namespace NovaTerm::Capabilities {
struct InvocationLease::State {
    std::function<std::optional<Rejection>()> authorize;
    CapabilityDispatcher::Now now;
    std::function<void()> cancel;
    TimePoint deadline;
    InvocationContext context;
    CapabilityId capability;
    bool cancelled{false}, completed{false}, stopped{false};
    std::optional<Rejection> check() const {
        if (completed) return Rejection::AlreadyCompleted;
        if (cancelled || stopped) return Rejection::Cancelled;
        if (now() >= deadline) return Rejection::DeadlineExceeded;
        return authorize();
    }
    std::optional<Rejection> finish() {
        const auto reason = check();
        completed = true;
        cancel = {};
        return reason;
    }
};
struct CapabilityDispatcher::State {
    using Key = std::pair<PrincipalId, OperationId>;
    std::map<Key, std::weak_ptr<InvocationLease::State>> operations;
    Now now;
    bool stopping{false};
    void sweep() {
        for (auto it = operations.begin(); it != operations.end();) {
            const auto live = it->second.lock();
            if (!live || live->completed) it = operations.erase(it); else ++it;
        }
    }
};
std::optional<Rejection> InvocationLease::check() const
{ const auto state = _state; return state ? state->check() : std::optional<Rejection>{Rejection::AlreadyCompleted}; }
std::optional<Rejection> InvocationLease::checkAuthorization() const
{
    const auto state = _state;
    return state ? state->authorize() : std::optional<Rejection>{Rejection::UntrustedPrincipal};
}
const InvocationContext& InvocationLease::context() const { assert(_state); return _state->context; }
CapabilityId InvocationLease::capability() const { return _state ? _state->capability : CapabilityId{}; }
bool InvocationLease::finish() { return _state && !_state->finish(); }
void InvocationLease::onCancel(std::function<void()> callback)
{
    if (!_state || _state->completed) return;
    if (_state->cancelled || _state->stopped) { if (callback) callback(); }
    else _state->cancel = std::move(callback);
}
void InvocationLease::setDeadline(TimePoint deadline) { if (_state && !_state->completed) _state->deadline = deadline; }
std::function<std::optional<Rejection>()> InvocationLease::completion() const
{
    return [state = _state]() -> std::optional<Rejection> {
        return state ? state->finish() : std::optional<Rejection>{Rejection::AlreadyCompleted};
    };
}
std::function<std::optional<Rejection>()> InvocationLease::validation() const
{
    return [state = _state]() -> std::optional<Rejection> {
        return state ? state->check() : std::optional<Rejection>{Rejection::AlreadyCompleted};
    };
}
CapabilityDispatcher::CapabilityDispatcher(const CapabilityRegistry& registry,
    const CapabilityAuthorization& auth, Now now)
    : _state(std::make_shared<State>()), _registry(registry), _authorization(auth)
{ _state->now = std::move(now); }
CapabilityDispatcher::~CapabilityDispatcher()
{
    // Destructors do not call arbitrary user callbacks. Owners may explicitly stop().
    for (const auto& item : _state->operations)
        if (const auto live = item.second.lock()) { live->stopped = true; live->cancel = {}; }
}
void CapabilityDispatcher::stop()
{
    _state->stopping = true;
    std::vector<std::function<void()>> callbacks;
    for (const auto& item : _state->operations) {
        if (const auto state = item.second.lock()) {
            state->stopped = true;
            if (state->cancel) callbacks.push_back(std::move(state->cancel));
        }
    }
    for (const auto& callback : callbacks) callback();
}
Result<InvocationLease> CapabilityDispatcher::admit(const CapabilityId& id, const InvocationContext& context)
{
    if (_state->stopping) return Rejection::Cancelled;
    const auto definition = _registry.find(id);
    if (!definition) return Rejection::Unavailable;
    if (context.operation.empty()) return Rejection::DuplicateOperation;
    if (_state->now() >= context.deadline) return Rejection::DeadlineExceeded;
    if (const auto reason = _authorization.validate(context, *definition)) return *reason;
    _state->sweep();
    const State::Key key{context.principal, context.operation};
    if (_state->operations.count(key)) return Rejection::DuplicateOperation;
    if (_state->operations.size() >= MaxOperations) return Rejection::Busy;
    auto state = std::make_shared<InvocationLease::State>();
    state->authorize = _authorization.validator(context, *definition);
    state->now = _state->now;
    state->deadline = context.deadline;
    state->context = context;
    state->capability = id;
    _state->operations.emplace(key, state);
    return InvocationLease{std::move(state)};
}
bool CapabilityDispatcher::cancel(const PrincipalId& principal, const OperationId& operation)
{
    const auto it = _state->operations.find({principal, operation});
    if (it == _state->operations.end()) return false;
    const auto live = it->second.lock();
    if (!live || live->completed || live->cancelled) return false;
    live->cancelled = true;
    const auto callback = std::move(live->cancel);
    if (callback) callback();
    return true;
}
size_t CapabilityDispatcher::activeCount() const { _state->sweep(); return _state->operations.size(); }
}
