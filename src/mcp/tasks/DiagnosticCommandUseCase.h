/**
 * @file DiagnosticCommandUseCase.h
 * @brief 固定诊断命令（execute_command）用例；Service 只做依赖装配。
 */
#pragma once

#include "mcp/application/CommandDedup.h"
#include "ExecutionRegistry.h"
#include "Job.h"
#include "ScriptTaskRegistry.h"
#include "mcp/CommandPolicy.h"
#include "session/CommandExecutionTypes.h"
#include "session/SessionDirectory.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class SessionCommandFacade;

namespace NovaTerm::Mcp {

class AccessStore;
class TargetGuard;

class DiagnosticCommandUseCase
{
public:
    struct Ports
    {
        AccessStore* access{nullptr};
        ExecutionRegistry* executions{nullptr};
        ScriptTaskRegistry* scriptTasks{nullptr};
        TargetGuard* guards{nullptr};
        CommandDedup* dedup{nullptr};
        QSet<SessionCommandFacade*>* observedFacades{nullptr};
        quint64* nextRequest{nullptr};
        QString instance;
        std::function<qint64()> now;
        QObject* context{nullptr};
        std::function<void(const std::shared_ptr<Job>&, QJsonObject)> complete;
        std::function<void()> changed;
        std::function<void(const CommandExecutionResult&)> finished;
    };

    explicit DiagnosticCommandUseCase(Ports ports);

    void run(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry,
        const QString& client, const QByteArray& connectionKey);

private:
    Ports _ports;
};

} // namespace NovaTerm::Mcp
