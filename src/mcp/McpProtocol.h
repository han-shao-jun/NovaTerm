/**
 * @file McpProtocol.h
 * @brief MCP的限额、格式、签名和安全文件工具。
 */
#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <chrono>
#include <optional>

namespace NovaTerm::Mcp {
/** @brief 编码后响应的上限；请求另用 MaxRequestBytes。 */
inline constexpr qsizetype MaxFrameBytes = 2 * 1024 * 1024;
inline constexpr qsizetype MaxOutputQueueBytes = 4 * 1024 * 1024;
inline constexpr qsizetype MaxTextBytes = 256 * 1024;
inline constexpr int MaxClients = 4;
inline constexpr int MaxActiveRequests = 2;
inline constexpr int MaxQueuedRequests = 4;
inline constexpr int MaxCaptures = 4;
inline constexpr int CaptureLifetimeMs = 60000;
inline constexpr int CommandOutputBytes = 65536;
inline constexpr int MaxTerminalActions = 64;
inline constexpr qsizetype MaxTerminalActionTextBytes = 16 * 1024;
inline constexpr int MaxTerminalWaitMs = 30000;
inline constexpr qsizetype MaxScriptBytes = 2 * 1024 * 1024;
/** @brief JSON 字符串最坏每 UTF-8 字节展开为六字节；其余有界字段另留信封额度。 */
inline constexpr qsizetype MaxRequestEnvelopeBytes = 256 * 1024;
inline constexpr qsizetype MaxRequestBytes = 6 * MaxScriptBytes + MaxRequestEnvelopeBytes;
inline constexpr auto ConfirmationTimeout = std::chrono::milliseconds{60000};
inline constexpr auto ScriptWriteTimeout = std::chrono::milliseconds{120000};
inline constexpr auto WireDeadlineMargin = std::chrono::milliseconds{5000};
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
[[nodiscard]] QJsonArray tools();
/** @return 空表示合法，否则为参数错误说明；不接受未知工具和未知字段。 */
[[nodiscard]] QString validateArguments(const QString& name, const QJsonObject& arguments);
[[nodiscard]] bool secureDirectory(const QString& path);
[[nodiscard]] bool secureFile(const QString& path);
[[nodiscard]] bool writePrivateJson(const QString& path, const QJsonObject& object);
[[nodiscard]] std::optional<QJsonObject> readJson(const QString& path, qsizetype maxBytes = 65536);
[[nodiscard]] QString runtimeDirectory();
[[nodiscard]] QString dataDirectory();
}
