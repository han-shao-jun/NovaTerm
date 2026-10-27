#include "SavedTargets.h"
#include <QUuid>

namespace NovaTerm::Mcp {
namespace {
QString opaqueVersion() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validText(const QString& value, int maximum)
{
    if (value.isEmpty() || value.toUtf8().size() > maximum
        || QString::fromUtf8(value.toUtf8()) != value) return false;
    for (const auto ch : value)
        if (ch.unicode() < 32 || ch.unicode() == 127) return false;
    return true;
}
}
SavedTargets::SavedTargets(QObject* parent) : QObject(parent), _revision(opaqueVersion()) {}
void SavedTargets::invalidate() { _revision = opaqueVersion(); emit changed(); }
bool SavedTargets::replace(const QList<SavedTargetSource>& sources)
{
    QMap<QString, Entry> next;
    bool valid = sources.size() <= 256;
    for (const auto& source : sources) {
        if (!valid || QUuid(source.id).isNull()
            || QUuid(source.id).toString(QUuid::WithoutBraces) != source.id
            || next.contains(source.id) || !validText(source.title, 256)
            || !validText(source.endpoint, 256) || source.configurationKey.size() != 32
            || (source.transport != "ssh" && source.transport != "serial")) {
            valid = false; break;
        }
        const auto old = _targets.constFind(source.id);
        const bool unchanged = old != _targets.cend() && old->configurationKey == source.configurationKey
            && old->value.title == source.title && old->value.transport == source.transport
            && old->value.endpoint == source.endpoint;
        next.insert(source.id, {{source.id, source.title, source.transport, source.endpoint,
            unchanged ? old->value.configVersion : opaqueVersion()}, source.configurationKey});
    }
    if (!valid) next.clear(); // Invalid replacement cannot leave a stale shared snapshot visible.
    bool changed = next.size() != _targets.size();
    for (auto it = next.cbegin(); !changed && it != next.cend(); ++it) {
        const auto old = _targets.constFind(it.key());
        changed = old == _targets.cend() || old->value.configVersion != it->value.configVersion;
    }
    if (!changed) return valid;
    _targets = std::move(next);
    for (auto client = _grants.begin(); client != _grants.end();) {
        for (auto grant = client->begin(); grant != client->end();) {
            const auto target = _targets.constFind(grant.key());
            if (target == _targets.cend() || target->value.configVersion != grant->configVersion)
                grant = client->erase(grant);
            else ++grant;
        }
        if (client->isEmpty()) client = _grants.erase(client); else ++client;
    }
    invalidate();
    return valid;
}
QList<SavedTarget> SavedTargets::localTargets() const
{
    QList<SavedTarget> rows;
    for (const auto& entry : _targets) rows.append(entry.value);
    return rows;
}
bool SavedTargets::isShared(const QString& client, quint64 version, const QString& id) const
{
    const auto grants = _grants.constFind(client);
    if (version == 0 || grants == _grants.cend()) return false;
    const auto grant = grants->constFind(id);
    const auto target = _targets.constFind(id);
    return target != _targets.cend() && grant != grants->cend()
        && grant->identityVersion == version && grant->configVersion == target->value.configVersion;
}
bool SavedTargets::setShared(const QString& client, quint64 version, const QString& id,
    const QString& expected, bool selected)
{
    const auto target = _targets.constFind(id);
    if (!validText(client, 64) || version == 0 || target == _targets.cend()
        || target->value.configVersion != expected) return false;
    auto grants = _grants.find(client);
    if (selected) {
        if (isShared(client, version, id)) return true;
        qsizetype count = 0;
        for (const auto& entries : _grants) count += entries.size();
        const bool existing = grants != _grants.end() && grants->contains(id);
        if (!existing && count >= 4096) return false;
        _grants[client].insert(id, {version, expected});
    } else {
        if (grants == _grants.end() || !grants->remove(id)) return true;
        if (grants->isEmpty()) _grants.erase(grants);
    }
    invalidate();
    return true;
}
void SavedTargets::revoke(const QString& client)
{
    if (client.isEmpty()) {
        if (!_grants.isEmpty()) { _grants.clear(); invalidate(); }
        return;
    }
    if (_grants.remove(client)) invalidate();
}
SavedTargetPage SavedTargets::page(const QString& client, quint64 version, int limit,
    const QString& after, const QString& expected) const
{
    SavedTargetPage result; result.revision = _revision;
    if (client.isEmpty() || !version || limit < 1 || limit > 50) {
        result.error = "INVALID_ARGUMENT"; return result;
    }
    if (!expected.isEmpty() && expected != _revision) {
        result.error = "TARGET_DIRECTORY_CHANGED"; return result;
    }
    for (auto it = _targets.upperBound(after); it != _targets.cend(); ++it) {
        if (!isShared(client, version, it.key())) continue;
        if (result.targets.size() == limit) {
            result.nextAfter = result.targets.last().id; break;
        }
        result.targets.append(it->value);
    }
    return result;
}
}
