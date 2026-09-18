/** @file LocalMcpServer.h
 *  @brief 专用 I/O 线程的本机 IPC 服务，收发队列有界且合并 GUI 唤醒。
 */
#pragma once
#include <QJsonObject>
#include <QObject>
#include <memory>

namespace NovaTerm::Mcp {
class LocalMcpServer final : public QObject
{
    Q_OBJECT
public:
    explicit LocalMcpServer(QObject* parent = nullptr);
    ~LocalMcpServer() override;
    void start(const QString& endpoint);
    void stop();
    bool send(const QString& connectionId, const QJsonObject& message);
    void closeConnection(const QString& connectionId);
signals:
    void listening(bool ok);
    void packet(const QString& connectionId, const QJsonObject& message);
    void disconnected(const QString& connectionId);
private slots:
    void drain();
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
