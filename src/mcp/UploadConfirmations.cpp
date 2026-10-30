#include "UploadConfirmations.h"
#include "McpProtocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>

namespace NovaTerm::Mcp {
namespace {
constexpr qint64 Lifetime = 5 * 60 * 1000;
QString digest(const QJsonArray& values)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
QString memoryKey(const UploadConfirmations::Intent& i)
{
    return digest({i.client, i.memoryIdentity.isEmpty() ? i.identity : i.memoryIdentity,
        i.targetKey, i.localPath, i.remotePath, i.protocol, i.overwrite});
}
QString signature(const UploadConfirmations::Intent& i)
{
    return digest({memoryKey(i), i.identity, i.connection, i.binding, i.sessionId, i.epoch, i.operationId});
}
UploadConfirmations::Decision rejected(const char* code)
{
    return {false, failure(QString::fromLatin1(code), QString::fromLatin1(code))};
}
}
UploadConfirmations::UploadConfirmations(QString directory, Now now)
    : _file(QDir(directory).filePath("upload-memory.json")), _now(std::move(now)) { _clock.start(); }

UploadConfirmations::Decision UploadConfirmations::check(const Intent& intent,
    const QString& token, std::optional<bool> confirmed)
{
    const auto now = _now ? _now() : _clock.elapsed();
    for (auto it = _pending.begin(); it != _pending.end();)
        if (it->expires <= now) it = _pending.erase(it); else ++it;
    const auto key = digest({intent.client, intent.identity, intent.operationId});
    const auto sig = signature(intent);
    auto found = _pending.find(key);
    if (found != _pending.end() && found->signature != sig) return rejected("TARGET_OPERATION_CONFLICT");
    if (confirmed.has_value() && (token.isEmpty() || found == _pending.end() || found->token != token))
        return rejected("UPLOAD_CONFIRMATION_STALE");
    if (!confirmed && !token.isEmpty()) return rejected("UPLOAD_CONFIRMATION_STALE");
    // Lock and reload per call so another app instance's clear/confirmation is
    // observed. Only hashes are persisted; pending tokens stay in this process.
    if (!secureDirectory(QFileInfo(_file).absolutePath())) return rejected("UPLOAD_MEMORY_UNAVAILABLE");
    QLockFile lock(_file + ".lock");
    if (!lock.tryLock(0)) return rejected("UPLOAD_MEMORY_BUSY");
    QJsonArray remembered;
    const auto stored = readJson(_file, 65536);
    QString generation;
    if (stored && stored->value("version").toInt() == 1) {
        generation = stored->value("generation").toString();
        const auto entries = stored->value("entries").toArray();
        if (entries.size() <= 512) for (const auto& entry : entries) {
            const auto text = entry.toString();
            if (text.size() == 64 && QByteArray::fromHex(text.toLatin1()).size() == 32) remembered.append(text);
        }
    }
    if (generation.isEmpty()) {
        generation = newId();
        if (!writePrivateJson(_file, {{"version", 1}, {"generation", generation}, {"entries", remembered}}))
            return rejected("UPLOAD_MEMORY_UNAVAILABLE");
    }
    if (found != _pending.end() && found->generation != generation) {
        _pending.erase(found); found = _pending.end();
        if (confirmed) return rejected("UPLOAD_CONFIRMATION_STALE");
    }
    if (found != _pending.end() && found->answer.has_value()) {
        if (confirmed && *confirmed != *found->answer) return rejected("UPLOAD_CONFIRMATION_STALE");
        if (*found->answer) return {true, {}};
    }
    const auto memory = memoryKey(intent);
    if (found == _pending.end() && !confirmed && remembered.contains(memory)) return {true, {}};
    if (found != _pending.end() && !confirmed && !found->answer && remembered.contains(memory)) return {true, {}};
    if (found == _pending.end()) {
        if (_pending.size() >= 64) return rejected("BUSY");
        found = _pending.insert(key, Pending{intent, sig, memory, QString::fromLatin1(randomBytes().toHex()), generation, now + Lifetime, {}});
    }
    if (confirmed) {
        if (*confirmed) {
            if (!remembered.contains(memory)) {
                while (remembered.size() >= 512) remembered.removeFirst();
                remembered.append(memory);
            }
            if (!writePrivateJson(_file, {{"version", 1}, {"generation", generation}, {"entries", remembered}}))
                return rejected("UPLOAD_MEMORY_UNAVAILABLE");
            found->answer = true;
            return {true, {}};
        }
        found->answer = false;
    }
    const bool declined = found->answer == std::optional<bool>(false);
    return {false, success({{"state", declined ? "declined" : "confirmation_required"},
        {"sessionId", intent.sessionId}, {"epoch", intent.epoch}, {"operationId", intent.operationId},
        {"localPath", intent.localPath}, {"remotePath", intent.remotePath}, {"target", intent.target},
        {"protocol", intent.protocol}, {"overwrite", intent.overwrite}, {"confirmationToken", found->token},
        {"expiresInMs", double(found->expires - now)},
        {"message", declined ? "Upload declined. No file was sent."
            : "Ask the user once whether to upload this exact localPath to this target/remotePath with the shown overwrite choice. After acceptance repeat the same tool arguments and operationId with this confirmationToken and confirmed=true. Use confirmed=false to decline. Do not ask again when a remembered request starts immediately."}})};
}
bool UploadConfirmations::clear()
{
    if (!secureDirectory(QFileInfo(_file).absolutePath())) return false;
    QLockFile lock(_file + ".lock");
    if (!lock.tryLock(0) || !writePrivateJson(_file, {{"version", 1}, {"generation", newId()}, {"entries", QJsonArray{}}})) return false;
    _pending.clear();
    return true;
}
void UploadConfirmations::disconnect(const QString& connection)
{
    for (auto it = _pending.begin(); it != _pending.end();)
        if (it->intent.connection == connection) it = _pending.erase(it); else ++it;
}
void UploadConfirmations::invalidate() { _pending.clear(); }
}
