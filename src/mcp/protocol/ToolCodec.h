/**
 * @file ToolCodec.h
 * @brief 工具 Schema、参数校验、强类型解码与错误编码的唯一边界。
 *
 * 职责边界（2026-10-18 运行期重构设计 §5）：只做协议形状到业务值的转换；
 * 不授权、不访问 Session、不持有任务状态。
 */
#pragma once

#include "mcp/contracts/Errors.h"
#include "mcp/contracts/ToolRequests.h"
#include "ToolCall.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace NovaTerm::Mcp {

class ToolCodec
{
public:
    ToolCodec() = delete;

    /** @brief 现行工具目录；已退役的旧命令工具不再公布。 */
    [[nodiscard]] static const QJsonArray& catalog();

    [[nodiscard]] static bool isKnownTool(const QString& name);

    /**
     * @brief 兼容入口：逐字段校验参数。
     * @return 空字符串表示合法，否则为对外拒绝说明（INVALID_ARGUMENT）。
     */
    [[nodiscard]] static QString validate(const QString& name, const QJsonObject& arguments);

    /**
     * @brief 现行终端领域解码；完整协议调用使用 decodeCall。
     *
     * 返回 `Error{InvalidArgument}` 时 `message` 与 `validate()` 的文本一致，
     * 保证外部错误报文不变。
     */
    [[nodiscard]] static Result<ToolRequest> decode(
        const QString& name, const QJsonObject& arguments);
    /** @brief 协议组合边界：分别解码终端/历史 DTO，不扩张终端应用请求集合。 */
    [[nodiscard]] static Result<ToolCall> decodeCall(const QString& name, const QJsonObject& arguments);

    /** @brief 编码 `{schemaVersion, ok:false, error}` 失败封装。 */
    [[nodiscard]] static QJsonObject encodeFailure(const Error& error);

    /** @brief 请求对应的工具名；用于日志、记录与迁移期路由。 */
    [[nodiscard]] static QString toolName(const ToolRequest& request);
    [[nodiscard]] static QString toolName(const ToolCall& request);
    /** @brief 把 typed 动作编码回规范化 wire 形状；用于幂等摘要与提交。 */
    [[nodiscard]] static QJsonArray encodeActions(
        const std::vector<TerminalAction>& actions);

    /** @brief 确认声明的唯一 JSON/MAC 边界，保持已有 requestState 格式。 */
    [[nodiscard]] static ConfirmationAnswer confirmationAnswer(const QString& action, bool confirmed);
    [[nodiscard]] static std::optional<ConfirmationAnswer> confirmationAnswer(const QJsonObject& responses);
};

} // namespace NovaTerm::Mcp
