/**
 * @file ExecutionRegistry.h
 * @brief 执行任务记录的单一索引；按执行 ID 与 Executor 请求 ID 查找。
 *
 * 记录只保存绑定、策略/目标、Executor 句柄、所属作业键与终态载荷；不复制
 * Job 的可变状态。未知结果由 `complete` 标志与载荷表达，淘汰只按完成时间。
 */
#pragma once

#include "mcp/contracts/TaskPhases.h"

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include <cstddef>

class SessionCommandCoordinator;
class SessionCommandFacade;

namespace NovaTerm::Mcp {

struct ExecutionRecord
{
    QString id;
    QString connection;
    QString client;
    QString session;
    QString epoch;
    QString commandId;
    QString policyVersion;
    QString target;
    quint64 executorRequestId{0};
    qint64 submitted{0};
    QPointer<SessionCommandFacade> facade;
    QPointer<SessionCommandCoordinator> coordinator;
    QString jobKey; ///< 所属作业索引键；作业完成后清空。
    QJsonObject payload;
    bool complete{false};
    bool interactive{false};
    QString scriptPathLock;
    TaskPhase phase{TaskPhase::Preparing};
};

class ExecutionRegistry
{
public:
    void insert(const QString& id, ExecutionRecord record);
    [[nodiscard]] ExecutionRecord* find(const QString& id);
    [[nodiscard]] const ExecutionRecord* find(const QString& id) const;
    [[nodiscard]] ExecutionRecord* findByRequest(quint64 requestId);
    [[nodiscard]] const ExecutionRecord* findByRequest(quint64 requestId) const;
    [[nodiscard]] QVector<ExecutionRecord> records() const;
    [[nodiscard]] int countForConnection(const QString& connection) const;
    [[nodiscard]] int countForClient(const QString& client) const;
    [[nodiscard]] std::size_t size() const;
    bool remove(const QString& id);
    /** @brief 仅当当前阶段与 `from` 一致时推进到 `to`；返回是否推进。 */
    bool advancePhase(const QString& id, TaskPhase from, TaskPhase to);
    /** @brief 淘汰完成且超过保留期的记录，返回淘汰数量。 */
    int expireCompleted(qint64 now, qint64 retentionMs);
    void clear();

private:
    QHash<QString, ExecutionRecord> _records;
};

} // namespace NovaTerm::Mcp
