/**
 * @file McpProtocol.h
 * @brief MCP/私有 IPC 共用的限额、格式、签名和安全文件工具。
 */
#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace NovaTerm::Mcp {
inline constexpr qsizetype MaxFrameBytes = 2 * 1024 * 1024;
inline constexpr qsizetype MaxOutputQueueBytes = 4 * 1024 * 1024;
inline constexpr qsizetype MaxTextBytes = 256 * 1024;
inline constexpr int MaxClients = 4;
inline constexpr int MaxActiveRequests = 2;
inline constexpr int MaxQueuedRequests = 4;
inline constexpr int MaxCaptures = 4;
inline constexpr int CaptureLifetimeMs = 60000;
inline constexpr int CommandOutputBytes = 65536;
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
[[nodiscard]] QJsonArray tools();
/** @return 空表示合法，否则为参数错误说明；不接受未知工具和未知字段。 */
[[nodiscard]] QString validateArguments(const QString& name, const QJsonObject& arguments);
[[nodiscard]] bool secureDirectory(const QString& path);
[[nodiscard]] bool secureFile(const QString& path);
[[nodiscard]] bool writePrivateJson(const QString& path, const QJsonObject& object);
[[nodiscard]] std::optional<QJsonObject> readJson(const QString& path, qsizetype maxBytes = 65536);
[[nodiscard]] QString runtimeDirectory();
[[nodiscard]] QString dataDirectory();
[[nodiscard]] QByteArray frame(const QJsonObject& object);

/** @brief 有界长度前缀解析器；调用方按 capacity 读取，避免一次 readAll 无界分配。 */
class FrameReader final
{
public:
    [[nodiscard]] qsizetype capacity() const { return MaxFrameBytes + 4 - _bytes.size(); }
    bool append(const QByteArray& bytes);
    [[nodiscard]] std::optional<QJsonObject> take();
    [[nodiscard]] bool failed() const { return _failed; }
private:
    QByteArray _bytes;
    bool _failed{false};
};
}
