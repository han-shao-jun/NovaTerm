#pragma once
#include "CapabilityDispatcher.h"
namespace NovaTerm::Capabilities {
template<class Response> using Completion = std::function<void(Result<Response>)>;
// A domain owns its request/result and handler. The dispatcher never sees payloads.
template<class Request, class Response> class TypedCapabilityPort
{
public:
    using Handler = std::function<void(Request, InvocationLease, Completion<Response>)>;
    TypedCapabilityPort(CapabilityDispatcher& dispatcher, CapabilityId capability, Handler handler)
        : _dispatcher(dispatcher), _capability(std::move(capability)), _handler(std::move(handler)) {}
    void invoke(Request request, const InvocationContext& context, Completion<Response> done) const
    {
        auto admitted = _dispatcher.admit(_capability, context);
        if (const auto* reason = std::get_if<Rejection>(&admitted)) { done(*reason); return; }
        auto lease = std::move(std::get<InvocationLease>(admitted));
        auto finish = lease.completion();
        const auto handler = _handler; // Delivery may destroy the port that owns the handler.
        handler(std::move(request), std::move(lease),
            [finish = std::move(finish), done = std::move(done)](Result<Response> result) {
                const auto reason = finish();
                if (reason == Rejection::AlreadyCompleted) return;
                if (reason) done(*reason); else done(std::move(result));
            });
    }
private:
    CapabilityDispatcher& _dispatcher;
    CapabilityId _capability;
    Handler _handler;
};
}
