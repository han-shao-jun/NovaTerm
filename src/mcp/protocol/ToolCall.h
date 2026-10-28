#pragma once
#include "mcp/contracts/ToolRequests.h"
#include "HistoryToolRequest.h"
#include "mcp/contracts/ClearRequest.h"
#include "mcp/contracts/WaitTextRequest.h"
#include "mcp/contracts/TerminalTaskRequest.h"
#include "mcp/contracts/SavedTargetRequest.h"
#include "mcp/contracts/FileResourceRequest.h"
#include "mcp/contracts/FileTransferRequest.h"
#include "mcp/contracts/SerialFileTargetRequest.h"
namespace NovaTerm::Mcp {
// Protocol composition envelope; this type never enters terminal application ports.
using ToolCall = std::variant<ToolRequest, HistoryToolRequest, ClearRequest, WaitTextRequest, TerminalTaskRequest, ListSavedTargetsRequest, TargetConnectionRequest, ListFileResourcesRequest, FileTransferRequest, ListSerialFileTargetsRequest>;
}
