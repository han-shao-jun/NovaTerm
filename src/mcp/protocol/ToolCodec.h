/**
 * @file ToolCodec.h
 * @brief 九工具 Schema、参数校验、强类型解码与错误编码的唯一边界。
 *
 * 职责边界（2026-10-18 运行期重构设计 §5）：只做协议形状到业务值的转换；
 * 不授权、不访问 Session、不持有任务状态。
 */
#pragma once

#include "mcp/contracts/Errors.h"
#include "mcp/contracts/ToolRequests.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace NovaTerm::Mcp {

class ToolCodec
{
public:
    ToolCodec() = delete;

    /** @brief 九工具目录；与重构前 `tools()` 完全相同的对象内容。 */
    [[nodiscard]] static const QJsonArray& catalog();
    /** @brief Published catalog: legacy nine objects plus independent history tools. */
    [[nodiscard]] static const QJsonArray& publishedCatalog();

    [[nodiscard]] static bool isKnownTool(const QString& name);

    /**
     * @brief 兼容入口：逐字段校验参数。
     * @return 空字符串表示合法，否则为对外拒绝说明（INVALID_ARGUMENT）。
     */
    [[nodiscard]] static QString validate(const QString& name, const QJsonObject& arguments);

    /**
     * @brief 校验并解码为强类型请求。
     *
     * 返回 `Error{InvalidArgument}` 时 `message` 与 `validate()` 的文本一致，
     * 保证外部错误报文不变。
     */
    [[nodiscard]] static Result<ToolRequest> decode(
        const QString& name, const QJsonObject& arguments);

    /** @brief 编码 `{schemaVersion, ok:false, error}` 失败封装。 */
    [[nodiscard]] static QJsonObject encodeFailure(const Error& error);

    /** @brief 请求对应的工具名；用于日志、记录与迁移期路由。 */
    [[nodiscard]] static QString toolName(const ToolRequest& request);
    /** @brief 把 typed 动作编码回规范化 wire 形状；用于幂等摘要与提交。 */
    [[nodiscard]] static QJsonArray encodeActions(
        const std::vector<TerminalAction>& actions);

    /** @brief 确认声明的唯一 JSON/MAC 边界，保持已有 requestState 格式。 */
    [[nodiscard]] static QJsonObject encodeConfirmation(const ConfirmationTransaction& transaction);
    [[nodiscard]] static ConfirmationAnswer confirmationAnswer(const QString& action, bool confirmed);
    [[nodiscard]] static std::optional<ConfirmationAnswer> confirmationAnswer(const QJsonObject& responses);
    [[nodiscard]] static std::optional<ConfirmationTransaction> decodeConfirmation(const QJsonObject& claims);
    [[nodiscard]] static QString signConfirmation(const ConfirmationTransaction& transaction,
        const QByteArray& key);
    [[nodiscard]] static std::optional<ConfirmationTransaction> verifyConfirmation(
        const QString& token, const QByteArray& key);
};

} // namespace NovaTerm::Mcp
