/**
 * @file ScriptResultUseCase.h
 * @brief 脚本发布结果的收口：校验、执行记录、预留释放与命令交接。
 */
#pragma once

#include "ExecutionRegistry.h"
#include "Job.h"
#include "ScriptTaskRegistry.h"
#include "TaskRegistry.h"
#include "session/SessionDirectory.h"

#include <QString>

#include <functional>
#include <memory>

namespace NovaTerm::Mcp {

class AccessStore;
class TargetGuard;

class ScriptResultUseCase
{
public:
    struct Ports
    {
        AccessStore* access{nullptr};
        ExecutionRegistry* executions{nullptr};
        ScriptTaskRegistry* scriptTasks{nullptr};
        TaskRegistry* jobs{nullptr};
        SessionDirectory* sessions{nullptr};
        QString instance;
        std::function<qint64()> now;
        std::function<void()> changed;
        std::function<void(const std::shared_ptr<Job>&, QJsonObject)> complete;
        std::function<void(const std::shared_ptr<Job>&, bool)> releaseReservation;
        std::function<void(const std::shared_ptr<Job>&,
            const SessionDirectory::Entry&)> runCommand;
    };

    explicit ScriptResultUseCase(Ports ports);

    void handleResult(const ScriptWriteResult& result);
    void recordOutcome(const ScriptTaskRegistry::Task& task,
        const ScriptWriteResult& result);

private:
    Ports _ports;
};

} // namespace NovaTerm::Mcp
