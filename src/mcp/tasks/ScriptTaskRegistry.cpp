/** @file ScriptTaskRegistry.cpp @brief 脚本任务与路径锁存储实现。 */
#include "ScriptTaskRegistry.h"

#include <utility>

namespace NovaTerm::Mcp {

void ScriptTaskRegistry::insert(quint64 id, Task task)
{
    _tasks.insert(id, std::move(task));
}

ScriptTaskRegistry::Task* ScriptTaskRegistry::find(quint64 id)
{
    const auto found = _tasks.find(id);
    return found == _tasks.end() ? nullptr : &found.value();
}

const ScriptTaskRegistry::Task* ScriptTaskRegistry::find(quint64 id) const
{
    const auto found = _tasks.constFind(id);
    return found == _tasks.cend() ? nullptr : &found.value();
}

std::optional<ScriptTaskRegistry::Task> ScriptTaskRegistry::take(quint64 id)
{
    const auto found = _tasks.find(id);
    if (found == _tasks.end())
        return std::nullopt;
    auto task = std::move(found.value());
    _tasks.erase(found);
    return task;
}

bool ScriptTaskRegistry::remove(quint64 id) { return _tasks.remove(id) != 0; }

QList<quint64> ScriptTaskRegistry::ids() const { return _tasks.keys(); }

QVector<ScriptTaskRegistry::Task> ScriptTaskRegistry::tasks() const
{
    QVector<Task> result;
    result.reserve(_tasks.size());
    for (const auto& task : _tasks)
        result.append(task);
    return result;
}

std::size_t ScriptTaskRegistry::size() const { return std::size_t(_tasks.size()); }

bool ScriptTaskRegistry::containsJob(const std::shared_ptr<Job>& job) const
{
    for (const auto& task : _tasks)
        if (task.job == job)
            return true;
    return false;
}

bool ScriptTaskRegistry::lockPath(const QString& path, const QString& owner)
{
    if (_pathLocks.contains(path))
        return false;
    _pathLocks.insert(path, owner);
    return true;
}

void ScriptTaskRegistry::unlockOwner(const QString& owner)
{
    for (auto it = _pathLocks.begin(); it != _pathLocks.end();) {
        if (it.value() == owner)
            it = _pathLocks.erase(it);
        else
            ++it;
    }
}

bool ScriptTaskRegistry::pathLocked(const QString& path) const
{
    return _pathLocks.contains(path);
}

void ScriptTaskRegistry::clear()
{
    _tasks.clear();
    _pathLocks.clear();
}

} // namespace NovaTerm::Mcp
