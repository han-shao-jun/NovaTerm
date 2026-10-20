#pragma once
#include "capabilities/CapabilityDispatcher.h"
#include "mcp/contracts/SessionBinding.h"
namespace NovaTerm::Mcp {
inline Capabilities::SessionBinding capabilityBinding(const SessionBinding& binding)
{
    return {Capabilities::SessionId{binding.session.str()}, Capabilities::AttachmentId{binding.attachment.str()},
        Capabilities::Epoch{binding.epoch.str()}, binding.generation.value(), binding.targetFingerprint};
}
}
