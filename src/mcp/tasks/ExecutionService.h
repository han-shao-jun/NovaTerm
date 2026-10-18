/**
 * @file ExecutionService.h
 * @brief 命令执行终态收口：结果投影、保护释放与作业完成。
 */
#pragma once

#include "ExecutionRegistry.h"
#include "Job.h"
#include "TaskRegistry.h"
#include "session/CommandExecutionTypes.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <memory>

namespace NovaTerm::Mcp {

class TargetGuard;

class ExecutionService
{
public:
    struct Ports
    {
        ExecutionRegistry* executions{nullptr};
        TargetGuard* guards{nullptr};
        TaskRegistry* jobs{nullptr};
        QString instance;
        std::function<void(const std::shared_ptr<Job>&, QJsonObject)> complete;
        std::function<void()> changed;
    };

    explicit ExecutionService(Ports ports);
    void finish(const CommandExecutionResult& result);

private:
    Ports _ports;
};

} // namespace NovaTerm::Mcp
