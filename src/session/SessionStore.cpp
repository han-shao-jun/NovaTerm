/**
 * @file   SessionStore.cpp
 * @brief  会话恢复元数据持久化实现。
 *
 * RuntimeConfig 与 SessionRestoreMetadata 的 JSON 序列化/反序列化在此完成。
 * 写入前通过 ProfileStore::containsSensitiveValues 拦截含凭据的元数据，
 * 防止敏感信息泄露到磁盘。
 */
#include "SessionStore.h"

#include "profile/ProfileStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
QJsonObject runtimeToJson(const RuntimeConfig& config)
{
    return {{QStringLiteral("schemaVersion"), config.schemaVersion},
            {QStringLiteral("profileId"), config.profileId},
            {QStringLiteral("credentialRef"), config.credentialRef},
            {QStringLiteral("title"), config.title},
            {QStringLiteral("transportKind"), static_cast<int>(config.transportKind)},
            {QStringLiteral("transport"), QJsonObject::fromVariantMap(config.transport)},
            {QStringLiteral("presentationDefaults"),
             QJsonObject::fromVariantMap(config.presentationDefaults)}};
}

RuntimeConfig runtimeFromJson(const QJsonObject& object)
{
    RuntimeConfig result;
    result.schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt();
    result.profileId = object.value(QStringLiteral("profileId")).toString();
    result.credentialRef = object.value(QStringLiteral("credentialRef")).toString();
    result.title = object.value(QStringLiteral("title")).toString();
    result.transportKind = static_cast<TransportKind>(
        object.value(QStringLiteral("transportKind")).toInt());
    result.transport = object.value(QStringLiteral("transport")).toObject().toVariantMap();
    result.presentationDefaults = object.value(
        QStringLiteral("presentationDefaults")).toObject().toVariantMap();
    return result;
}
}

SessionStore::SessionStore(QString filePath)
    : _filePath(std::move(filePath))
{
}

bool SessionStore::save(const QList<SessionRestoreMetadata>& sessions,
                        QString* error) const
{
    QJsonArray array;
    for (const auto& session : sessions) {
        if (ProfileStore::containsSensitiveValues(session.overrides)
            || ProfileStore::containsSensitiveValues(session.runtimeSnapshot.transport)) {
            if (error)
                *error = QStringLiteral("session metadata contains credential data");
            return false;
        }
        array.append(QJsonObject{
            {QStringLiteral("sessionId"), session.sessionId.toString(QUuid::WithoutBraces)},
            {QStringLiteral("profileId"), session.profileId},
            {QStringLiteral("overrides"), QJsonObject::fromVariantMap(session.overrides)},
            {QStringLiteral("runtime"), runtimeToJson(session.runtimeSnapshot)},
            {QStringLiteral("reconnectOnRestore"), session.reconnectOnRestore}});
    }
    QSaveFile file(_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

QList<SessionRestoreMetadata> SessionStore::load(QString* error) const
{
    QFile file(_filePath);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error)
            *error = parseError.errorString();
        return {};
    }
    QList<SessionRestoreMetadata> result;
    for (const auto& value : document.array()) {
        const auto object = value.toObject();
        SessionRestoreMetadata item;
        item.sessionId = QUuid(object.value(QStringLiteral("sessionId")).toString());
        item.profileId = object.value(QStringLiteral("profileId")).toString();
        item.overrides = object.value(QStringLiteral("overrides")).toObject().toVariantMap();
        item.runtimeSnapshot = runtimeFromJson(
            object.value(QStringLiteral("runtime")).toObject());
        item.reconnectOnRestore = object.value(
            QStringLiteral("reconnectOnRestore")).toBool(true);
        result.append(std::move(item));
    }
    return result;
}
