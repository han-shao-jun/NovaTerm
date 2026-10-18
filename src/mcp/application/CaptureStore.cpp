/** @file CaptureStore.cpp @brief capture 作用域计额、FIFO 淘汰与过滤删除实现。 */
#include "CaptureStore.h"

#include <algorithm>
#include <utility>

namespace NovaTerm::Mcp {

CaptureStore::CaptureStore(CaptureQuotas quotas) : _quotas(quotas) {}

void CaptureStore::put(QString id, CaptureRecord record)
{
    if (record.scope == CaptureScope::Client) {
        if (size(CaptureScope::Client, record.owner) >= std::size_t(_quotas.perClient))
            eraseOldest(CaptureScope::Client, record.owner, false);
        if (size(CaptureScope::Client) >= std::size_t(_quotas.clientScopeTotal))
            eraseOldest(CaptureScope::Client, {}, true);
    } else if (size(CaptureScope::Connection, record.owner)
        >= std::size_t(_quotas.perConnection)) {
        eraseOldest(CaptureScope::Connection, record.owner, false);
    }
    record.sequence = ++_sequence;
    _records.insert(std::move(id), std::move(record));
}

std::optional<CaptureRecord> CaptureStore::find(const QString& id) const
{
    const auto record = _records.constFind(id);
    if (record == _records.cend())
        return std::nullopt;
    return *record;
}

bool CaptureStore::contains(const QString& id) const { return _records.contains(id); }

bool CaptureStore::ownedByClient(const QString& id, const QString& client) const
{
    const auto record = _records.constFind(id);
    return record != _records.cend() && record->scope == CaptureScope::Client
        && record->owner == client;
}

bool CaptureStore::remove(const QString& id) { return _records.remove(id) != 0; }

void CaptureStore::removeSession(const QString& session)
{
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->session == session)
            it = _records.erase(it);
        else
            ++it;
    }
}

void CaptureStore::removeConnection(const QString& connection)
{
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->scope == CaptureScope::Connection && it->owner == connection)
            it = _records.erase(it);
        else
            ++it;
    }
}

void CaptureStore::removeConnectionSession(const QString& connection, const QString& session)
{
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->scope == CaptureScope::Connection && it->owner == connection
            && it->session == session) {
            it = _records.erase(it);
        } else {
            ++it;
        }
    }
}

void CaptureStore::removeClient(const QString& client)
{
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->scope == CaptureScope::Client && it->owner == client)
            it = _records.erase(it);
        else
            ++it;
    }
}

void CaptureStore::removeClientSession(const QString& client, const QString& session)
{
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->scope == CaptureScope::Client && it->owner == client
            && it->session == session) {
            it = _records.erase(it);
        } else {
            ++it;
        }
    }
}

void CaptureStore::sweep(qint64 now)
{
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->expires <= now)
            it = _records.erase(it);
        else
            ++it;
    }
}

void CaptureStore::clear() { _records.clear(); }

std::size_t CaptureStore::size() const { return std::size_t(_records.size()); }

std::size_t CaptureStore::size(CaptureScope scope, const QString& owner) const
{
    return countIf([scope, &owner](const CaptureRecord& record) {
        return record.scope == scope && record.owner == owner;
    });
}

std::size_t CaptureStore::size(CaptureScope scope) const
{
    return countIf([scope](const CaptureRecord& record) {
        return record.scope == scope;
    });
}

std::size_t CaptureStore::countIf(
    const std::function<bool(const CaptureRecord&)>& predicate) const
{
    std::size_t count = 0;
    for (const auto& record : _records)
        if (predicate(record))
            ++count;
    return count;
}

void CaptureStore::eraseOldest(CaptureScope scope, const QString& owner, bool anyOwner)
{
    auto oldest = _records.end();
    for (auto it = _records.begin(); it != _records.end(); ++it) {
        if (it->scope != scope || (!anyOwner && it->owner != owner))
            continue;
        if (oldest == _records.end() || it->sequence < oldest->sequence)
            oldest = it;
    }
    if (oldest != _records.end())
        _records.erase(oldest);
}

} // namespace NovaTerm::Mcp
