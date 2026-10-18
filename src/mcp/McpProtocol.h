/**
 * @file McpProtocol.h
 * @brief MCP的限额、格式、签名和安全文件工具。
 */
#pragma once
#include "mcp/contracts/Budget.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <chrono>
#include <optional>

namespace NovaTerm::Mcp {
/**
 * @brief 协议层沿用的旧常量名；取值统一在 `contracts/Budget.h`。
 *
 * 迁移期保留这些别名，避免同一数字散落在函数里；新代码直接使用
 * `NovaTerm::Mcp::Budget`。
 */
inline constexpr qsizetype MaxFrameBytes = Budget::MaxFrameBytes;
inline constexpr qsizetype MaxOutputQueueBytes = Budget::MaxOutputQueueBytes;
inline constexpr qsizetype MaxTextBytes = Budget::MaxTextBytes;
inline constexpr int MaxClients = Budget::MaxClients;
inline constexpr int MaxActiveRequests = Budget::MaxActiveRequests;
inline constexpr int MaxQueuedRequests = Budget::MaxQueuedRequests;
inline constexpr int MaxCaptures = Budget::MaxCapturesPerConnection;
inline constexpr int CaptureLifetimeMs = int(Budget::CaptureLifetime.count());
inline constexpr int CommandOutputBytes = Budget::CommandOutputBytes;
inline constexpr int MaxTerminalActions = Budget::MaxTerminalActions;
inline constexpr qsizetype MaxTerminalActionTextBytes = Budget::MaxTerminalActionTextBytes;
inline constexpr int MaxTerminalWaitMs = Budget::MaxTerminalWaitMs;
inline constexpr qsizetype MaxScriptBytes = Budget::MaxScriptBytes;
inline constexpr qsizetype MaxRequestEnvelopeBytes = Budget::MaxRequestEnvelopeBytes;
inline constexpr qsizetype MaxRequestBytes = Budget::MaxRequestBytes;
inline constexpr auto ConfirmationTimeout = Budget::ConfirmationTimeout;
inline constexpr auto ScriptWriteTimeout = Budget::ScriptWriteTimeout;
inline constexpr auto WireDeadlineMargin = Budget::WireDeadlineMargin;
inline constexpr const char* ProtocolVersion = "2025-11-25";

[[nodiscard]] QString newId();
[[nodiscard]] QByteArray randomBytes(int count = 32);
[[nodiscard]] bool constantTimeEqual(const QByteArray& a, const QByteArray& b);
[[nodiscard]] QString signToken(const QJsonObject& fields, const QByteArray& key);
[[nodiscard]] std::optional<QJsonObject> verifyToken(const QString& token, const QByteArray& key);
[[nodiscard]] QByteArray clipUtf8(const QByteArray& text, qsizetype maximum);
[[nodiscard]] QString boundedText(const QString& text, qsizetype maximum, bool* truncated = nullptr);
[[nodiscard]] QString outputText(const QByteArray& bytes, qsizetype& remaining, bool& truncated);
[[nodiscard]] QJsonObject success(const QJsonObject& data);
[[nodiscard]] QJsonObject failure(const QString& code, const QString& message,
                                 bool retryable = false, int retryAfterMs = 0);
[[nodiscard]] QJsonObject toolResult(const QJsonObject& payload);
[[nodiscard]] QJsonObject rpcResult(const QJsonValue& id, const QJsonObject& result);
[[nodiscard]] QJsonObject rpcError(const QJsonValue& id, int code, const QString& message);
/** @brief 返回空对象表示有效 MCP 消息，否则返回待发送的 JSON-RPC 错误。 */
[[nodiscard]] QJsonObject rpcValidationError(const QJsonObject& message);
/** @brief 两种传输分流共用的版本拒绝错误，字段遵守 2026 schema。 */
[[nodiscard]] QJsonObject unsupportedProtocolError(const QJsonValue& id, const QString& requested);
[[nodiscard]] bool secureDirectory(const QString& path);
[[nodiscard]] bool secureFile(const QString& path);
[[nodiscard]] bool writePrivateJson(const QString& path, const QJsonObject& object);
[[nodiscard]] std::optional<QJsonObject> readJson(const QString& path, qsizetype maxBytes = 65536);
[[nodiscard]] QString runtimeDirectory();
[[nodiscard]] QString dataDirectory();
}
