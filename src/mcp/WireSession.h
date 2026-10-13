/** @file WireSession.h @brief MCP wire 协议适配，不持有传输或 Session 指针。 */
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <memory>
namespace NovaTerm::Mcp
{
class WireSession final : public QObject
{
    Q_OBJECT
public:
    explicit WireSession(QString wirePrefix = {}, QObject *parent = nullptr);
    ~WireSession() override;
    void receiveBytes(QByteArray bytes, QString route = {});
    void receiveMessage(QJsonObject message, QString route = {});
    void receiveBackend(QJsonObject message);
    void setBackendAvailable(bool available);
    void instanceUnavailable();
    void failPending(QString code);
    void cancelAll();
    [[nodiscard]] bool initialized() const;
    [[nodiscard]] bool hasPendingCalls() const;
signals:
    void backendMessage(QJsonObject message);
    /** @brief 最终 RPC 已编码；错误码单独传递，接线无需再次解析正文。 */
    void packet(QString route, QByteArray message, bool final, int errorCode);
    void completed(QString route);
    void attachmentRequested();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace NovaTerm::Mcp
