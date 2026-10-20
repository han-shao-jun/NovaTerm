#pragma once
#include "ToolRequests.h"
namespace NovaTerm::Mcp {
// Payload invariants shared by wire and typed local entrypoints; empty means valid.
QString validateTerminalActions(const std::vector<TerminalAction>& actions);
}
