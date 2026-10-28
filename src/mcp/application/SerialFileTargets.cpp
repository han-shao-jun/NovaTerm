#include "SerialFileTargets.h"
#include "mcp/contracts/Budget.h"
#include <QUuid>

namespace NovaTerm::Mcp {
namespace {
QString opaque() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validTitle(const QString& title)
{
    const auto utf8 = title.toUtf8();
    if (title.isEmpty() || utf8.size() > Budget::MaxSerialFileTargetTitleBytes
        || QString::fromUtf8(utf8) != title) return false;
    for (const auto ch : title) if (ch.category() == QChar::Other_Control) return false;
    return true;
}
}
SerialFileTargets::SerialFileTargets(Identity identity, QObject* parent) : QObject(parent), _identity(std::move(identity)) {}
bool SerialFileTargets::validIdentity(const QString& client, quint64 identity) const
{
    return !client.isEmpty() && identity && _identity && _identity(client) == identity;
}
bool SerialFileTargets::sameBinding(const SerialFileBinding& a, const SerialFileBinding& b)
{
    return a.connection.session == b.connection.session && a.connection.attachment == b.connection.attachment
        && a.connection.epoch == b.connection.epoch && a.connection.generation == b.connection.generation
        && a.connection.targetFingerprint == b.connection.targetFingerprint
        && a.ioGeneration == b.ioGeneration && a.configDigest == b.configDigest;
}
std::optional<SerialFileTarget> SerialFileTargets::share(const QString& client, quint64 identity,
    const SerialFileBinding& binding, const QString& title)
{
    if (!validIdentity(client, identity) || !validTitle(title) || binding.connection.session.isEmpty()
        || binding.connection.attachment.isEmpty() || binding.connection.epoch.isEmpty()
        || !binding.connection.generation.value() || !binding.ioGeneration || binding.configDigest.isEmpty()) return std::nullopt;
    int count = 0;
    for (auto it = _grants.begin(); it != _grants.end(); ++it) {
        if (it->client != client) continue;
        ++count;
        if (it->identityVersion != identity || !sameBinding(it->target.binding, binding)) continue;
        const SerialFileTarget target{it->target.id, title, opaque(), binding};
        it->target = target;
        emit changed(); return target;
    }
    if (count >= Budget::MaxSerialFileTargetsPerClient || _grants.size() >= Budget::MaxSerialFileTargets) return std::nullopt;
    const SerialFileTarget target{opaque(), title, opaque(), binding};
    _grants.insert(target.id, {client, identity, target});
    emit changed(); return target;
}
QList<SerialFileTarget> SerialFileTargets::list(const QString& client, quint64 identity) const
{
    QList<SerialFileTarget> result;
    if (validIdentity(client, identity)) for (const auto& grant : _grants)
        if (grant.client == client && grant.identityVersion == identity) result.append(grant.target);
    return result;
}
bool SerialFileTargets::remove(const QString& client, quint64 identity, const QString& id, const QString& revision)
{
    if (!validIdentity(client, identity)) return false;
    const auto it = _grants.find(id);
    if (it == _grants.end() || it->client != client || it->identityVersion != identity
        || it->target.revision != revision) return false;
    _grants.erase(it); emit changed(); return true;
}
void SerialFileTargets::revoke(const QString& client)
{
    bool removed = false;
    for (auto it = _grants.begin(); it != _grants.end();)
        if (client.isEmpty() || it->client == client) { it = _grants.erase(it); removed = true; } else ++it;
    if (removed) emit changed();
}
void SerialFileTargets::invalidateSession(const QString& session)
{
    bool removed = false;
    for (auto it = _grants.begin(); it != _grants.end();)
        if (it->target.binding.connection.session.str() == session) { it = _grants.erase(it); removed = true; } else ++it;
    if (removed) emit changed();
}
void SerialFileTargets::invalidateBinding(const SerialFileBinding& binding)
{
    bool removed = false;
    for (auto it = _grants.begin(); it != _grants.end();)
        if (sameBinding(it->target.binding, binding)) { it = _grants.erase(it); removed = true; } else ++it;
    if (removed) emit changed();
}
QList<SerialFileBinding> SerialFileTargets::bindings() const
{
    QList<SerialFileBinding> result;
    for (const auto& grant : _grants) result.append(grant.target.binding);
    return result;
}
}
