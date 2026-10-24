#pragma once
#include "ResourceSharingService.h"
#include "capabilities/TypedCapabilityPort.h"

namespace NovaTerm::Resources {
// Both MCP and trusted local callers enter the same domain admission boundary.
class HistoryCapabilities final {
public:
    HistoryCapabilities(Capabilities::CapabilityDispatcher&, ResourceSharingService&);
    Capabilities::TypedCapabilityPort<CaptureListQuery, CaptureListPage> list;
    Capabilities::TypedCapabilityPort<Recording::HistoryReadRequest, Recording::HistoryReadPage> read;
    Capabilities::TypedCapabilityPort<Recording::HistorySearchRequest, EvidenceSearchPage> search;
};
}
