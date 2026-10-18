/** @file ExecutionRegistry.cpp @brief 执行任务记录索引实现。 */
#include "ExecutionRegistry.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionCommandFacade.h"

#include <utility>

namespace NovaTerm::Mcp {

void ExecutionRegistry::insert(const QString& id, ExecutionRecord record)
{
    Q_ASSERT(id == record.id);
    if (id != record.id) return;
    _records.insert(id, std::move(record));
}

ExecutionRecord* ExecutionRegistry::find(const QString& id)
{
    const auto found = _records.find(id);
    return found == _records.end() ? nullptr : &found.value();
}

const ExecutionRecord* ExecutionRegistry::find(const QString& id) const
{
    const auto found = _records.constFind(id);
    return found == _records.cend() ? nullptr : &found.value();
}

ExecutionRecord* ExecutionRegistry::findByRequest(quint64 requestId)
{
    for (auto it = _records.begin(); it != _records.end(); ++it)
        if (it->executorRequestId == requestId)
            return &it.value();
    return nullptr;
}

const ExecutionRecord* ExecutionRegistry::findByRequest(quint64 requestId) const
{
    for (auto it = _records.cbegin(); it != _records.cend(); ++it)
        if (it->executorRequestId == requestId)
            return &it.value();
    return nullptr;
}

QVector<ExecutionRecord> ExecutionRegistry::records() const
{
    QVector<ExecutionRecord> result;
    result.reserve(_records.size());
    for (const auto& record : _records)
        result.append(record);
    return result;
}

int ExecutionRegistry::countForConnection(const QString& connection) const
{
    int count = 0;
    for (const auto& record : _records)
        if (record.connection == connection)
            ++count;
    return count;
}

int ExecutionRegistry::countForClient(const QString& client) const
{
    int count = 0;
    for (const auto& record : _records)
        if (record.client == client)
            ++count;
    return count;
}

std::size_t ExecutionRegistry::size() const { return std::size_t(_records.size()); }

bool ExecutionRegistry::remove(const QString& id) { return _records.remove(id) != 0; }

bool ExecutionRegistry::advancePhase(const QString& id, TaskPhase from, TaskPhase to)
{
    auto record = _records.find(id);
    if (record == _records.end() || record->phase != from
        || !taskPhaseCanAdvance(from, to))
        return false;
    record->phase = to;
    return true;
}

int ExecutionRegistry::expireCompleted(qint64 now, qint64 retentionMs)
{
    int removed = 0;
    for (auto it = _records.begin(); it != _records.end();) {
        if (it->complete && now - it->submitted >= retentionMs) {
            it = _records.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

void ExecutionRegistry::clear() { _records.clear(); }

} // namespace NovaTerm::Mcp
