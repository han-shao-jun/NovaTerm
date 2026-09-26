/** @file ElicitationBroker.h
 *  @brief MCP 客户端人工确认请求的有界关联器。
 */
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <optional>

class ElicitationBroker final : public QObject
{
    Q_OBJECT
public:
    struct Response {
        QString wireId;
        QString action;
        bool confirmed{false};
    };

    explicit ElicitationBroker(QObject* parent = nullptr);

    /** @brief 创建最多一个在途的 boolean 人工确认请求。 */
    [[nodiscard]] std::optional<QJsonObject> requestConfirmation(
        const QString& wireId, const QString& message);
    /** @brief 解析并消费对应的 JSON-RPC 反向响应。 */
    [[nodiscard]] std::optional<Response> handleResponse(
        const QJsonObject& response);
    /** @brief 丢弃指定工具调用的确认关联。 */
    void forget(const QString& wireId);

private:
    QHash<QString, QString> _reverseToWire;
};
