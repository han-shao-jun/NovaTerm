/** @file TaskRegistry.cpp @brief 在途任务注册表实现。 */
#include "TaskRegistry.h"

#include <utility>

namespace NovaTerm::Mcp {

void TaskRegistry::insert(const QString& key, std::shared_ptr<Job> job)
{
    _jobs.insert(key, std::move(job));
}

std::shared_ptr<Job> TaskRegistry::find(const QString& key) const
{
    return _jobs.value(key);
}

bool TaskRegistry::contains(const QString& key) const { return _jobs.contains(key); }

bool TaskRegistry::remove(const QString& key) { return _jobs.remove(key) != 0; }

bool TaskRegistry::empty() const { return _jobs.isEmpty(); }

std::size_t TaskRegistry::size() const { return std::size_t(_jobs.size()); }

QList<std::shared_ptr<Job>> TaskRegistry::snapshot() const
{
    QList<std::shared_ptr<Job>> result;
    result.reserve(_jobs.size());
    for (const auto& job : _jobs)
        result.append(job);
    return result;
}

void TaskRegistry::clear() { _jobs.clear(); }

} // namespace NovaTerm::Mcp
