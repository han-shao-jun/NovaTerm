#pragma once
#include "capabilities/TypedCapabilityPort.h"
#include "mcp/application/CaptureSearch.h"
#include "mcp/contracts/ToolRequests.h"
#include "mcp/contracts/Errors.h"
namespace NovaTerm::Capabilities::Terminal {
struct ContextSearchResult {
    Mcp::CaptureId capture;
    QString revision;
    Mcp::CaptureSearchResult search;
    bool sourceTruncated{false}, outputTruncated{false};
};
// Lookup owns capture provenance/TTL/scope; runner owns its bounded execution mailbox.
class SearchCapabilities
{
public:
    using Lookup = std::function<Mcp::Result<Mcp::CaptureRecord>(const InvocationContext&, const Mcp::SearchContextRequest&)>;
    using Runner = std::function<void(Mcp::CaptureRecord, Mcp::SearchContextRequest,
        std::function<void(Mcp::CaptureSearchResult)>)>;
    SearchCapabilities(CapabilityDispatcher&, Lookup, Runner);
    TypedCapabilityPort<Mcp::SearchContextRequest, Mcp::Result<ContextSearchResult>> search;
};
}
