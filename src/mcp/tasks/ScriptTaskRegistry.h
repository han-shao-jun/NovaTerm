/**
 * @file ScriptTaskRegistry.h
 * @brief 脚本发布任务与脚本路径锁的唯一所有者。
 *
 * Service 只按请求 ID 查询/取出任务；路径锁按作业键解除，避免两处平行容器
 * 在取消、Provider 消失与完成路径上失配。
 */
#pragma once

#include "session/ISessionScriptProvider.h"
#include "Job.h"

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QString>
#include <QVector>

#include <cstddef>
#include <memory>
#include <optional>

namespace NovaTerm::Mcp {

class ScriptTaskRegistry
{
public:
    struct Task
    {
        std::shared_ptr<Job> job;
        QPointer<ISessionScriptProvider> provider;
        QString client, session, epoch, targetFingerprint;
        QString policyVersion, profileVersion, targetPath, workingDirectory;
        QString invocation, scriptPathLock;
        QByteArray metadataHash, contentHash;
        quint64 promptGeneration{0}, grantVersion{0}, sessionGeneration{0};
        quint64 userInputGeneration{0};
    };

    void insert(quint64 id, Task task);
    [[nodiscard]] Task* find(quint64 id);
    [[nodiscard]] const Task* find(quint64 id) const;
    [[nodiscard]] std::optional<Task> take(quint64 id);
    bool remove(quint64 id);
    [[nodiscard]] QList<quint64> ids() const;
    [[nodiscard]] QVector<Task> tasks() const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool containsJob(const std::shared_ptr<Job>& job) const;

    /** @brief 路径锁；已有其他 owner 时返回 false。 */
    bool lockPath(const QString& path, const QString& owner);
    void unlockOwner(const QString& owner);
    [[nodiscard]] bool pathLocked(const QString& path) const;

    void clear();

private:
    QHash<quint64, Task> _tasks;
    QHash<QString, QString> _pathLocks; ///< path → 作业键
};

} // namespace NovaTerm::Mcp
