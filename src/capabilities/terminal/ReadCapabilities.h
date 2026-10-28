#pragma once
#include "capabilities/TypedCapabilityPort.h"
#include "mcp/tasks/ReadUseCases.h"
namespace NovaTerm::Capabilities::Terminal {
struct SessionListQuery { int limit{50}; std::optional<Mcp::DirectoryCursor> cursor; bool humanConfirmation{false}; };
struct ContextReadQuery { Mcp::ContextQuery query; std::optional<Mcp::ContextCursor> cursor; };
// Both entry adapters consume these exact ports. No JSON, HTTP, tokens or Session objects.
class ReadCapabilities
{
public:
    ReadCapabilities(CapabilityDispatcher&, const Mcp::ReadSessionPort&);
    TypedCapabilityPort<SessionListQuery, Mcp::Result<Mcp::SessionPage>> sessions;
    TypedCapabilityPort<ContextReadQuery, Mcp::Result<Mcp::ContextPage>> context;
};
}
