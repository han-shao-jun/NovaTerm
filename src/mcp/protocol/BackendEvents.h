/**
 * @file BackendEvents.h
 * @brief Wire 向业务后端传递统一工具请求或取消事件。
 *
 * 协议确认兼容性在 Wire 边界处理，业务调度只接收已解码的工具参数。
 */
#pragma once

#include "ToolCall.h"
#include <QMetaType>
#include <QString>

namespace NovaTerm::Mcp {

struct BackendCall
{
    QString id;
    ToolCall request;
};

struct BackendCancel
{
    QString id;
};

using BackendEvent = std::variant<BackendCall, BackendCancel>;

/** @brief 事件类别；仅用于日志与测试断言，不参与业务分派。 */
[[nodiscard]] inline QString backendEventKind(const BackendEvent& event)
{
    return std::holds_alternative<BackendCall>(event)
        ? QStringLiteral("call") : QStringLiteral("cancel");
}

} // namespace NovaTerm::Mcp

Q_DECLARE_METATYPE(NovaTerm::Mcp::BackendEvent)
