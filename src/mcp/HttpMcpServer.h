/**
 * @file HttpMcpServer.h
 * @brief 仅监听本机的 HTTP I/O 门面；不持有 MCP 或 Session 业务状态。
 */
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <memory>

namespace NovaTerm::Mcp
{

class HttpMcpServer final : public QObject
{
    Q_OBJECT
public:
    explicit HttpMcpServer(QObject *parent = nullptr);
    ~HttpMcpServer() override;

    /** @brief 在独立 I/O 线程监听；0 表示由系统分配端口。 */
    void start(quint16 port = 0);
    /** @brief 同步停止并回收 I/O 线程；不依赖 GUI 事件处理。 */
    void stop();
    /**
     * @brief 有界入队响应；返回 false 表示请求已结束或额度不足。
     * @note SSE 的空正文、final=false 可用于先开启响应流。
     * @note 非空 SSE 正文由 I/O 线程核验为 JSON 对象并组帧；无效正文断开。
     */
    [[nodiscard]] bool respond(QString requestId,
                               int status,
                               QByteArray body = {},
                               QJsonObject headers = {},
                               bool sse = false,
                               bool final = true);
    /** @brief 无正文、令牌和请求参数的线程安全诊断计数。 */
    [[nodiscard]] QJsonObject statistics() const;

signals:
    void listening(bool ok, QString url);
    void request(QString id, QJsonObject envelope);
    void disconnected(QString requestId);

private slots:
    void drain();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm::Mcp
