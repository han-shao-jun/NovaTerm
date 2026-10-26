/** @file TerminalTaskRequest.h @brief 长任务启动、查询及请求停止的独立契约。 */
#pragma once
#include "ToolRequests.h"
#include "Budget.h"

namespace NovaTerm::Mcp {
struct StartTerminalTaskRequest {
    SessionId session;
    Epoch epoch;
    QString inputToken;
    OperationId operation;
    std::vector<TerminalAction> actions;
    std::chrono::milliseconds timeout{Budget::TerminalTaskDefaultTimeout};
};
struct QueryTerminalTaskRequest {
    SessionId session;
    Epoch epoch;
    std::optional<TaskId> task;
    std::optional<OperationId> operation;
    std::optional<QString> sinceToken;
};
struct StopTerminalTaskRequest {
    SessionId session;
    Epoch epoch;
    TaskId task;
    OperationId operation;
};
using TerminalTaskRequest = std::variant<StartTerminalTaskRequest,
    QueryTerminalTaskRequest, StopTerminalTaskRequest>;
}
