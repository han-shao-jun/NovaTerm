/** @file WireSession.h @brief MCP wire 协议适配，不持有传输或 Session 指针。 */
#pragma once
#include "protocol/BackendEvents.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>
namespace NovaTerm::Mcp
{
class WireSession final : public QObject
{
    Q_OBJECT
public:
    /** @brief 可注入与会话同生命周期的单调毫秒时钟；默认使用 QElapsedTimer。 */
    explicit WireSession(QString wirePrefix = {}, QObject *parent = nullptr,
                         std::function<qint64()> monotonicClock = {});
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
    void backendEvent(const BackendEvent &event);
    /** @brief 最终 RPC 已编码；错误码单独传递，接线无需再次解析正文。 */
    void packet(QString route, QByteArray message, bool final, int errorCode);
    void completed(QString route);
    void attachmentRequested();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace NovaTerm::Mcp
