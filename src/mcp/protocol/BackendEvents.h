/**
 * @file BackendEvents.h
 * @brief Wire → 业务后端的类型化事件；不再使用内部 JSON `op` 总线。
 *
 * `BackendCall` / `BackendHistoryCall` 分别携带终端/历史工具参数；取消与确认结果只带
 * 关联 ID 与决定。响应方向仍由 WireSession 的 JSON-RPC 编码器处理。
 */
#pragma once

#include "mcp/contracts/ToolRequests.h"
#include "HistoryToolRequest.h"
#include "mcp/contracts/ClearRequest.h"
#include "mcp/contracts/WaitTextRequest.h"
#include "mcp/contracts/TerminalTaskRequest.h"
#include "mcp/contracts/FileResourceRequest.h"
#include "mcp/contracts/FileTransferRequest.h"
#include "mcp/contracts/SerialFileTargetRequest.h"
#include <QMetaType>
#include <QString>

#include <variant>

namespace NovaTerm::Mcp {

struct BackendCall
{
    QString id;
    ToolRequest request;
    bool mrtr{false};
    bool formElicitation{false};
    QString requestState;
    std::optional<ConfirmationAnswer> answer;
};

struct BackendCancel
{
    QString id;
};
struct BackendHistoryCall {
    QString id;
    HistoryToolRequest request;
    bool confirmationSupplied{false};
};
struct BackendClearCall { QString id; ClearRequest request; bool confirmationSupplied{false}; };
struct BackendWaitTextCall { QString id; WaitTextRequest request; bool confirmationSupplied{false}; };
struct BackendTaskCall { QString id; TerminalTaskRequest request; bool confirmationSupplied{false}; };
struct BackendFileResourcesCall { QString id; ListFileResourcesRequest request; bool confirmationSupplied{false}; };
struct BackendFileTransferCall { QString id; FileTransferRequest request; bool confirmationSupplied{false}; };
struct BackendSerialFileTransferCall { QString id; SerialFileTransferRequest request; bool confirmationSupplied{false}; };
struct BackendSerialFileTargetsCall { QString id; ListSerialFileTargetsRequest request; bool confirmationSupplied{false}; };

struct BackendElicitationResult
{
    QString id;
    ConfirmationAnswer answer{ConfirmationAnswer::Decline};
};

using BackendEvent = std::variant<BackendCall, BackendHistoryCall, BackendClearCall, BackendWaitTextCall, BackendTaskCall, BackendFileResourcesCall, BackendFileTransferCall, BackendSerialFileTargetsCall, BackendSerialFileTransferCall, BackendCancel, BackendElicitationResult>;

/** @brief 事件类别；仅用于日志与测试断言，不参与业务分派。 */
[[nodiscard]] inline QString backendEventKind(const BackendEvent& event)
{
    return std::visit([](const auto& value) -> QString {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, BackendCall> || std::is_same_v<Type, BackendHistoryCall>
            || std::is_same_v<Type, BackendClearCall> || std::is_same_v<Type, BackendWaitTextCall>
            || std::is_same_v<Type, BackendTaskCall>
            || std::is_same_v<Type, BackendFileResourcesCall>
            || std::is_same_v<Type, BackendFileTransferCall> || std::is_same_v<Type, BackendSerialFileTargetsCall>
            || std::is_same_v<Type, BackendSerialFileTransferCall>)
            return QStringLiteral("call");
        else if constexpr (std::is_same_v<Type, BackendCancel>)
            return QStringLiteral("cancel");
        else
            return QStringLiteral("elicitation_result");
    }, event);
}

} // namespace NovaTerm::Mcp

Q_DECLARE_METATYPE(NovaTerm::Mcp::BackendEvent)
