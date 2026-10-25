#pragma once
#include "TypedCapabilityPort.h"
namespace NovaTerm::Capabilities {
class McpCapabilityAdapter final
{
public:
    McpCapabilityAdapter(const CapabilityAuthorization& auth, PrincipalId id)
        : _authorization(auth), _id(std::move(id)), _principal(auth.trust(_id, EntryKind::Mcp)) {}
    McpCapabilityAdapter(const McpCapabilityAdapter&) = delete;
    McpCapabilityAdapter& operator=(const McpCapabilityAdapter&) = delete;
    void setEnabled(bool enabled) {
        if (_enabled == enabled) return;
        _enabled = enabled;
        _principal.close();
        if (enabled) _principal = _authorization.trust(_id, EntryKind::Mcp);
    }
    InvocationContext context(ResourceBinding binding, quint64 version, OperationId operation, TimePoint deadline) const {
        return _authorization.context(_principal, std::move(binding), version, std::move(operation), deadline);
    }
    template<class Request, class Response, class Done>
    void invoke(const TypedCapabilityPort<Request, Response>& port, Request request,
        ResourceBinding resource, quint64 version, OperationId operation, TimePoint deadline, Done done) const {
        port.invoke(std::move(request), context(std::move(resource), version, std::move(operation), deadline), std::move(done));
    }
    ~McpCapabilityAdapter() { _principal.close(); }
private:
    const CapabilityAuthorization& _authorization;
    PrincipalId _id;
    TrustedPrincipal _principal;
    bool _enabled{true};
};
}
