/** @file ClientRegistry.cpp @brief 客户端身份与版本的有界索引实现。 */
#include "ClientRegistry.h"

#include <algorithm>

namespace NovaTerm::Mcp {
namespace {
/** @brief 版本只增不减；计数器本身不提供隐式算术。 */
ClientAccessVersion nextVersion(ClientAccessVersion current)
{
    return ClientAccessVersion{current.value() + 1};
}
} // namespace

QList<ClientRecord> ClientRegistry::clients() const
{
    auto result = _records.values();
    std::sort(result.begin(), result.end(),
        [](const ClientRecord& left, const ClientRecord& right) {
            return left.label < right.label;
        });
    return result;
}

std::optional<ClientRecord> ClientRegistry::find(const ClientId& id) const
{
    const auto record = _records.constFind(id);
    if (record == _records.cend())
        return std::nullopt;
    return *record;
}

bool ClientRegistry::contains(const ClientId& id) const { return _records.contains(id); }

int ClientRegistry::size() const { return _records.size(); }

QByteArray ClientRegistry::digestFor(const ClientId& id) const
{
    return _records.value(id).digest;
}

ClientAccessVersion ClientRegistry::version(const ClientId& id) const
{
    return _records.value(id).version;
}

quint64 ClientRegistry::identityVersion(const ClientId& id) const
{
    const auto record = _records.constFind(id);
    return record == _records.cend() ? 0 : record->identityVersion;
}

ClientAccessVersion ClientRegistry::advance(const ClientId& id)
{
    auto& record = _records[id];
    record.version = nextVersion(record.version);
    return record.version;
}

void ClientRegistry::insert(ClientRecord record)
{
    if (record.id.isEmpty())
        return;
    if (!record.identityVersion)
        record.identityVersion = ++_identityRevision;
    else if (record.identityVersion > _identityRevision)
        _identityRevision = record.identityVersion;
    _records.insert(record.id, std::move(record));
}

bool ClientRegistry::remove(const ClientId& id) { return _records.remove(id) != 0; }

bool ClientRegistry::updateDigest(const ClientId& id, QByteArray digest, bool persistent)
{
    auto record = _records.find(id);
    if (record == _records.end())
        return false;
    record->digest = std::move(digest);
    record->persistent = persistent;
    record->version = nextVersion(record->version);
    record->identityVersion = ++_identityRevision;
    return true;
}

void ClientRegistry::replaceAll(QList<ClientRecord> records)
{
    _records.clear();
    for (auto& record : records)
        insert(std::move(record));
}

void ClientRegistry::advanceAll()
{
    for (auto record = _records.begin(); record != _records.end(); ++record)
        record->version = nextVersion(record->version);
}

void ClientRegistry::advanceAllIdentities()
{
    for (auto record = _records.begin(); record != _records.end(); ++record)
        record->identityVersion = ++_identityRevision;
}

void ClientRegistry::clear() { _records.clear(); }

} // namespace NovaTerm::Mcp
