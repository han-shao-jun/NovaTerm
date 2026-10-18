/**
 * @file TaskRegistry.h
 * @brief 在途 RPC 任务（Job）的唯一所有者；Service 只按索引键访问。
 *
 * 快照接口返回 `shared_ptr` 列表，供取消/失效遍历；遍历期间任务可同步完成，
 * 调用方必须按当前注册表状态重新校验，不得持有迭代器跨越回调。
 */
#pragma once

#include "Job.h"

#include <QHash>
#include <QList>
#include <QString>

#include <cstddef>
#include <memory>

namespace NovaTerm::Mcp {

class TaskRegistry
{
public:
    void insert(const QString& key, std::shared_ptr<Job> job);
    [[nodiscard]] std::shared_ptr<Job> find(const QString& key) const;
    [[nodiscard]] bool contains(const QString& key) const;
    bool remove(const QString& key);
    [[nodiscard]] bool empty() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] QList<std::shared_ptr<Job>> snapshot() const;
    void clear();

private:
    QHash<QString, std::shared_ptr<Job>> _jobs;
};

} // namespace NovaTerm::Mcp
