#pragma once
#include "mcp/contracts/ToolRequests.h"
#include "capabilities/CapabilityTypes.h"
namespace NovaTerm::Mcp {
// Terminal domain mapping only. New unrelated domains do not change this set.
template<class Request> const char* terminalCapabilityName();
template<> inline const char* terminalCapabilityName<ListSessionsRequest>() { return "session.list"; }
template<> inline const char* terminalCapabilityName<ReadContextRequest>() { return "terminal.context.read"; }
template<> inline const char* terminalCapabilityName<SearchContextRequest>() { return "terminal.context.search"; }
template<> inline const char* terminalCapabilityName<InteractTerminalRequest>() { return "terminal.interact"; }
template<> inline const char* terminalCapabilityName<RequestElevationRequest>() { return "terminal.elevation.request"; }
inline Capabilities::CapabilityId terminalCapability(const ToolRequest& request)
{
    return std::visit([](const auto& value) {
        return Capabilities::CapabilityId{QString::fromLatin1(terminalCapabilityName<std::decay_t<decltype(value)>>())};
    }, request);
}
}
