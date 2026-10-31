#pragma once
#include "capabilities/CapabilityDispatcher.h"
#include "mcp/contracts/SessionBinding.h"
#include "mcp/contracts/Errors.h"
namespace NovaTerm::Mcp {
inline Error capabilityError(Capabilities::Rejection rejection)
{
    switch (rejection) {
    case Capabilities::Rejection::DeadlineExceeded: return Error{ErrorCode::DeadlineExceeded};
    case Capabilities::Rejection::Cancelled: return Error{ErrorCode::Cancelled};
    case Capabilities::Rejection::Busy:
        return Error{ErrorCode::Busy, {}, RetryHint{true, std::chrono::milliseconds{250}}};
    case Capabilities::Rejection::Unavailable:
    case Capabilities::Rejection::UntrustedPrincipal:
    case Capabilities::Rejection::Denied:
    case Capabilities::Rejection::AuthorizationChanged:
    case Capabilities::Rejection::ResourceMismatch:
    case Capabilities::Rejection::AlreadyCompleted:
    case Capabilities::Rejection::DuplicateOperation:
        return Error{ErrorCode::SessionNotAvailable};
    }
    return Error{ErrorCode::SessionNotAvailable};
}
inline Capabilities::SessionBinding capabilityBinding(const SessionBinding& binding)
{
    return {Capabilities::SessionId{binding.session.str()}, Capabilities::AttachmentId{binding.attachment.str()},
        Capabilities::Epoch{binding.epoch.str()}, binding.generation.value(), binding.targetFingerprint};
}
}
