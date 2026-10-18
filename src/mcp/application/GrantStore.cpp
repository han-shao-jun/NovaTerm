/** @file GrantStore.cpp @brief 运行期授权行的有界索引实现。 */
#include "GrantStore.h"

namespace NovaTerm::Mcp {

std::optional<GrantRow> GrantStore::find(const ClientId& client, const SessionId& session) const
{
    const auto rows = _rows.constFind(client);
    if (rows == _rows.cend())
        return std::nullopt;
    const auto row = rows->constFind(session);
    if (row == rows->cend())
        return std::nullopt;
    return *row;
}

void GrantStore::save(const ClientId& client, const SessionId& session, GrantRow row)
{
    if (session.isEmpty())
        return;
    _rows[client].insert(session, std::move(row));
}

void GrantStore::removeSession(const SessionId& session)
{
    for (auto rows = _rows.begin(); rows != _rows.end();) {
        rows->remove(session);
        if (rows->isEmpty())
            rows = _rows.erase(rows);
        else
            ++rows;
    }
}

void GrantStore::removeClient(const ClientId& client) { _rows.remove(client); }

void GrantStore::clear() { _rows.clear(); }

std::vector<GrantEntry> GrantStore::entries() const
{
    std::vector<GrantEntry> result;
    for (auto clients = _rows.cbegin(); clients != _rows.cend(); ++clients) {
        for (auto rows = clients->cbegin(); rows != clients->cend(); ++rows)
            result.push_back(GrantEntry{clients.key(), rows.key(), *rows});
    }
    return result;
}

std::size_t GrantStore::size() const
{
    std::size_t count = 0;
    for (const auto& rows : _rows)
        count += std::size_t(rows.size());
    return count;
}

} // namespace NovaTerm::Mcp
