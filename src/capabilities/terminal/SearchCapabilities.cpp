#include "SearchCapabilities.h"
namespace NovaTerm::Capabilities::Terminal {
SearchCapabilities::SearchCapabilities(CapabilityDispatcher& dispatcher, Lookup lookup, Runner runner)
    : search(dispatcher, CapabilityId{"terminal.context.search"},
        [lookup = std::move(lookup), runner = std::move(runner)](Mcp::SearchContextRequest request,
            InvocationLease lease, Completion<Mcp::Result<ContextSearchResult>> done) {
            const auto* binding = std::get_if<SessionBinding>(&lease.context().resource);
            if (!binding || binding->session.value() != request.session.str() || binding->epoch.value() != request.epoch.str()) {
                done(Rejection::ResourceMismatch); return;
            }
            if (request.query.isEmpty() || request.query.toUtf8().size() > 1024 || request.maxMatches < 1 || request.maxMatches > 100) {
                done(Mcp::Result<ContextSearchResult>{Mcp::Error{Mcp::ErrorCode::InvalidArgument}}); return;
            }
            const auto found = lookup(lease.context(), request);
            if (const auto* error = Mcp::errorIfAny(found)) { done(Mcp::Result<ContextSearchResult>{*error}); return; }
            const auto& record = std::get<Mcp::CaptureRecord>(found);
            if (record.session != binding->session.value() || record.epoch != binding->epoch.value()
                || (record.scope == Mcp::CaptureScope::Client && (record.owner != lease.context().principal.value()
                    || record.grantVersion != lease.context().accessVersion))) {
                done(Mcp::Result<ContextSearchResult>{Mcp::Error{Mcp::ErrorCode::CaptureNotAvailable}}); return;
            }
            ContextSearchResult projection{request.capture, record.revision, {}, record.sourceTruncated, record.outputTruncated};
            runner(record, std::move(request), [projection = std::move(projection), done = std::move(done)](Mcp::CaptureSearchResult value) mutable {
                if (value.interrupted) { done(Mcp::Result<ContextSearchResult>{Mcp::Error{Mcp::ErrorCode::DeadlineExceeded}}); return; }
                projection.search = std::move(value);
                done(Mcp::Result<ContextSearchResult>{std::move(projection)});
            });
        })
{}
}
