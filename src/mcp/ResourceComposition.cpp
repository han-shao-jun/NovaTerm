#include "ResourceComposition.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include <QDir>
#include <QJsonArray>
#include <QLockFile>
#include <QPointer>
namespace NovaTerm::Mcp {
namespace {
struct GrantFile {
    QString path;
    std::unique_ptr<QLockFile> lock;
    bool writable{false};
    explicit GrantFile(const QString& state) : path(QDir(state).filePath("history-grants.json")) {
        if (!secureDirectory(state)) return;
        lock = std::make_unique<QLockFile>(QDir(state).filePath("history-grants.lock"));
        lock->setStaleLockTime(0); writable = lock->tryLock();
    }
    std::vector<Recording::ResourceGrant> load() const {
        std::vector<Recording::ResourceGrant> grants;
        if (!writable) return grants;
        const auto saved = readJson(path); if (!saved) return grants;
        const auto rows = saved->value("grants").toArray(); if (rows.size() > 128) return grants;
        for (const auto& value : rows) {
            const auto row = value.toObject(); Recording::ResourceGrant grant;
            grant.principalId = row.value("principal").toString(); grant.tokenEpoch = row.value("tokenEpoch").toString().toULongLong();
            grant.captureId = {QUuid(row.value("captureId").toString())}; grant.resourceGeneration = row.value("generation").toString().toULongLong();
            grant.firstRecordId = row.value("first").toString().toULongLong(); grant.lastRecordId = row.value("last").toString().toULongLong();
            grant.persistent = true; grants.push_back(grant);
        }
        return grants;
    }
    bool save(const std::vector<Recording::ResourceGrant>& grants) const {
        if (!writable) return false;
        QJsonArray rows;
        for (const auto& grant : grants) rows.append(QJsonObject{{"principal", grant.principalId},
            {"tokenEpoch", QString::number(grant.tokenEpoch)}, {"captureId", grant.captureId.text()},
            {"generation", QString::number(grant.resourceGeneration)}, {"first", QString::number(grant.firstRecordId)},
            {"last", QString::number(grant.lastRecordId)}});
        return writePrivateJson(path, {{"version", 1}, {"grants", rows}});
    }
};
}
std::shared_ptr<Resources::ResourceSharingService> makeResourceSharingService(AccessStore& access, const QString& state) {
    const QPointer<AccessStore> authority(&access);
    auto identity = [authority](const QString& principal) -> quint64 {
        if (authority) for (const auto& client : authority->clients()) if (client.id.str() == principal) {
            const auto epoch = client.digest.toHex().left(16).toULongLong(nullptr, 16); return epoch ? epoch : 1;
        }
        return 0;
    };
    const auto file = std::make_shared<GrantFile>(state);
    Resources::GrantPersistence persistence;
    if (file->writable) persistence = {[file] { return file->load(); }, [file](const auto& grants) { return file->save(grants); }};
    return std::make_shared<Resources::ResourceSharingService>(std::move(identity), std::move(persistence));
}
}
