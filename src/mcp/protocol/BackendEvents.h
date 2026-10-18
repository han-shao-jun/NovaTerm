/**
 * @file BackendEvents.h
 * @brief Wire → 业务后端的类型化事件；不再使用内部 JSON `op` 总线。
 *
 * `BackendCall` 携带已通过 Schema 校验的工具参数；取消与确认结果只带
 * 关联 ID 与决定。响应方向仍由 WireSession 的 JSON-RPC 编码器处理。
 */
#pragma once

#include "mcp/contracts/ToolRequests.h"
#include <QMetaType>
#include <QString>

#include <variant>

namespace NovaTerm::Mcp {

struct BackendCall
{
    QString id;
    ToolRequest request;
    QByteArray confirmationDigest;
    bool mrtr{false};
    bool formElicitation{false};
    QString requestState;
    std::optional<ConfirmationAnswer> answer;
};

struct BackendCancel
{
    QString id;
};

struct BackendElicitationResult
{
    QString id;
    ConfirmationAnswer answer{ConfirmationAnswer::Decline};
};

using BackendEvent = std::variant<BackendCall, BackendCancel, BackendElicitationResult>;

/** @brief 事件类别；仅用于日志与测试断言，不参与业务分派。 */
[[nodiscard]] inline QString backendEventKind(const BackendEvent& event)
{
    return std::visit([](const auto& value) -> QString {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, BackendCall>)
            return QStringLiteral("call");
        else if constexpr (std::is_same_v<Type, BackendCancel>)
            return QStringLiteral("cancel");
        else
            return QStringLiteral("elicitation_result");
    }, event);
}

} // namespace NovaTerm::Mcp

Q_DECLARE_METATYPE(NovaTerm::Mcp::BackendEvent)
