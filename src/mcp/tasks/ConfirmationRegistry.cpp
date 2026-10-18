/** @file ConfirmationRegistry.cpp @brief 确认 nonce 与待确认记录的存储实现。 */
#include "ConfirmationRegistry.h"

#include <algorithm>
#include <utility>

namespace NovaTerm::Mcp {

void ConfirmationRegistry::registerActive(const QString& connection, const QString& client,
    const QString& session, const QString& nonce, qint64 expiry)
{
    _active.insert(connection, nonce);
    _scopes.insert(nonce, Scope{connection, client, session, expiry});
}

void ConfirmationRegistry::clearActive(const QString& connection, const QString& nonce)
{
    if (_active.value(connection) == nonce)
        _active.remove(connection);
    _scopes.remove(nonce);
}

void ConfirmationRegistry::consumeActive(const QString& connection, const QString& nonce,
    qint64 expiry)
{
    _consumed.insert(nonce, expiry);
    clearActive(connection, nonce);
}

bool ConfirmationRegistry::isActive(const QString& connection, const QString& nonce) const
{
    return _active.value(connection) == nonce;
}

bool ConfirmationRegistry::hasActiveForConnection(const QString& connection) const
{
    return _active.contains(connection);
}

bool ConfirmationRegistry::isConsumed(const QString& nonce) const
{
    return _consumed.contains(nonce);
}

QString ConfirmationRegistry::takeConnection(const QString& connection)
{
    const QString nonce = _active.take(connection);
    _scopes.remove(nonce);
    return nonce;
}

void ConfirmationRegistry::removeScope(const QString& client, const QString& session)
{
    for (auto it = _scopes.begin(); it != _scopes.end();) {
        const bool matchesClient = client.isEmpty() || it->client == client;
        const bool matchesSession = session.isEmpty() || it->session == session;
        if (!matchesClient || !matchesSession) {
            ++it;
            continue;
        }
        if (_active.value(it->connection) == it.key())
            _active.remove(it->connection);
        it = _scopes.erase(it);
    }
}

void ConfirmationRegistry::insertPending(const QString& key, PendingConfirmationRecord record)
{
    _pending.insert(key, std::move(record));
}

std::optional<PendingConfirmationRecord> ConfirmationRegistry::takePending(const QString& key)
{
    const auto record = _pending.find(key);
    if (record == _pending.end())
        return std::nullopt;
    auto result = *record;
    _pending.erase(record);
    return result;
}

bool ConfirmationRegistry::removePending(const QString& key)
{
    return _pending.remove(key) != 0;
}

bool ConfirmationRegistry::hasPendingForConnection(const QString& connection) const
{
    return std::any_of(_pending.cbegin(), _pending.cend(),
        [&connection](const PendingConfirmationRecord& record) {
            return record.connection == connection;
        });
}

std::vector<PendingConfirmationRecord> ConfirmationRegistry::sweep(qint64 now)
{
    for (auto it = _consumed.begin(); it != _consumed.end();) {
        if (it.value() <= now)
            it = _consumed.erase(it);
        else
            ++it;
    }
    for (auto active = _active.begin(); active != _active.end();) {
        const auto scope = _scopes.constFind(active.value());
        if (scope == _scopes.cend() || scope->expires <= now) {
            _scopes.remove(active.value());
            active = _active.erase(active);
        } else {
            ++active;
        }
    }
    std::vector<PendingConfirmationRecord> expired;
    for (auto it = _pending.begin(); it != _pending.end();) {
        if (it->expires <= now) {
            expired.push_back(*it);
            it = _pending.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

bool ConfirmationRegistry::empty() const
{
    return _active.isEmpty() && _consumed.isEmpty() && _pending.isEmpty();
}

void ConfirmationRegistry::clear()
{
    _active.clear();
    _scopes.clear();
    _consumed.clear();
    _pending.clear();
}

} // namespace NovaTerm::Mcp
