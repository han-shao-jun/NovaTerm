#pragma once
#include "mcp/contracts/ToolRequests.h"
#include "HistoryToolRequest.h"
namespace NovaTerm::Mcp {
// Protocol composition envelope; this type never enters terminal application ports.
using ToolCall = std::variant<ToolRequest, HistoryToolRequest>;
}
