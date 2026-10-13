/** @file HttpMcpEndpoint.h
 *  @brief HTTP 请求与 MCP 授权上下文的 GUI 线程适配层。
 */
#pragma once
#include "HttpMcpServer.h"
#include <functional>
#include <memory>

namespace NovaTerm::Mcp
{
class HttpMcpEndpoint final : public QObject
{
    Q_OBJECT
public:
    struct Backend {
        std::function<QString(const QByteArray &)> authenticate;
        std::function<bool(const QString &, const QString &)> attach;
        std::function<void(const QString &, const QJsonObject &)> receive;
        std::function<void(const QString &)> detach;
    };
    explicit HttpMcpEndpoint(Backend backend, QObject *parent = nullptr);
    ~HttpMcpEndpoint() override;
    void start(quint16 port);
    void stop();
    [[nodiscard]] bool send(const QString &peer, const QJsonObject &message);
    void closeConnection(const QString &peer);
    [[nodiscard]] QJsonObject statistics() const;
signals:
    void listening(bool ok, QString url);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace NovaTerm::Mcp
