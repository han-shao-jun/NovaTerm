#include "HistoryCapabilities.h"
namespace NovaTerm::Resources {
namespace {
Recording::ResourceContext context(const Capabilities::InvocationLease& lease, const ResourceSharingService& resources) {
    return resources.context(lease.context().principal.value());
}
bool matches(const Capabilities::InvocationLease& lease, Recording::HistoryCaptureId id) {
    const auto* binding = std::get_if<Capabilities::HistoryCaptureBinding>(&lease.context().resource);
    return binding && binding->id.value() == id.text();
}
template<class Request> void bindCancellation(Request& request, Capabilities::InvocationLease& lease) {
    if (!request.cancelled) request.cancelled = std::make_shared<std::atomic<bool>>(false);
    lease.onCancel([cancelled = request.cancelled] { cancelled->store(true); });
    request.admitted = [validate = lease.validation(), restriction = std::move(request.admitted)] {
        return !validate() && (!restriction || restriction());
    };
}
}
HistoryCapabilities::HistoryCapabilities(Capabilities::CapabilityDispatcher& dispatcher, ResourceSharingService& resources)
    : list(dispatcher, Capabilities::CapabilityId{"history.capture.list"},
        [&resources](CaptureListQuery query, Capabilities::InvocationLease lease, auto done) {
            done(resources.list(context(lease, resources), query));
        })
    , read(dispatcher, Capabilities::CapabilityId{"history.raw.read"},
        [&resources](Recording::HistoryReadRequest request, Capabilities::InvocationLease lease, auto done) {
            if (!matches(lease, request.captureId)) { done(Capabilities::Rejection::ResourceMismatch); return; }
            bindCancellation(request, lease);
            resources.readAsync(std::move(request), context(lease, resources), [done = std::move(done)](auto page) { done(std::move(page)); });
        })
    , search(dispatcher, Capabilities::CapabilityId{"history.literal.search"},
        [&resources](Recording::HistorySearchRequest request, Capabilities::InvocationLease lease, auto done) {
            if (!matches(lease, request.captureId)) { done(Capabilities::Rejection::ResourceMismatch); return; }
            bindCancellation(request, lease);
            resources.searchAsync(std::move(request), context(lease, resources), [done = std::move(done)](auto page) { done(std::move(page)); });
        })
{}
}
