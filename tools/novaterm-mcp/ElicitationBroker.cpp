/** @file ElicitationBroker.cpp
 *  @brief 有界 MCP Elicitation 请求关联与响应校验。
 */
#include "ElicitationBroker.h"

#include <QJsonArray>
#include <QUuid>

ElicitationBroker::ElicitationBroker(QObject* parent)
    : QObject(parent)
{
}

std::optional<QJsonObject> ElicitationBroker::requestConfirmation(
    const QString& wireId, const QString& message)
{
    if (wireId.isEmpty() || message.isEmpty() || !_reverseToWire.isEmpty())
        return std::nullopt;

    const QString reverseId = QStringLiteral("nt-elicit-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    _reverseToWire.insert(reverseId, wireId);
    const QJsonObject schema{{"type", "object"},
        {"properties", QJsonObject{{"confirmed", QJsonObject{{"type", "boolean"}}}}},
        {"required", QJsonArray{"confirmed"}}, {"additionalProperties", false}};
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", reverseId},
        {"method", "elicitation/create"},
        {"params", QJsonObject{{"mode", "form"}, {"message", message},
            {"requestedSchema", schema}}}};
}

std::optional<ElicitationBroker::Response> ElicitationBroker::handleResponse(
    const QJsonObject& response)
{
    const QString reverseId = response.value("id").toString();
    const auto found = _reverseToWire.find(reverseId);
    if (found == _reverseToWire.end())
        return std::nullopt;

    Response resolved;
    resolved.wireId = found.value();
    _reverseToWire.erase(found);
    if (response.value("error").isObject()) {
        resolved.action = QStringLiteral("cancel");
        return resolved;
    }

    const auto result = response.value("result").toObject();
    const QString action = result.value("action").toString();
    resolved.confirmed = action == QStringLiteral("accept")
        && result.value("content").toObject().value("confirmed").toBool();
    resolved.action = action == QStringLiteral("cancel")
        ? QStringLiteral("cancel")
        : resolved.confirmed ? QStringLiteral("accept")
                             : QStringLiteral("decline");
    return resolved;
}

void ElicitationBroker::forget(const QString& wireId)
{
    for (auto it = _reverseToWire.begin(); it != _reverseToWire.end();) {
        if (it.value() == wireId)
            it = _reverseToWire.erase(it);
        else
            ++it;
    }
}
