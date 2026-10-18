/**
 * @file ScriptCommandUseCase.h
 * @brief 脚本发布与自由命令执行的用例层；Service 只做组合与回调装配。
 */
#pragma once

#include "ExecutionRegistry.h"
#include "Job.h"
#include "ScriptTaskRegistry.h"
#include "TaskRegistry.h"
#include "mcp/application/CommandDedup.h"
#include "session/CommandExecutionTypes.h"
#include "session/SessionDirectory.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

class ISessionScriptProvider;
class SessionCommandCoordinator;
class SessionCommandFacade;
class SessionInputArbiter;
class TerminalSession;

namespace NovaTerm::Mcp {

class AccessStore;
class ConfirmationRegistry;
class TargetGuard;

class ScriptCommandUseCase
{
public:
    struct Ports
    {
        AccessStore* access{nullptr};
        ExecutionRegistry* executions{nullptr};
        ScriptTaskRegistry* scriptTasks{nullptr};
        TaskRegistry* jobs{nullptr};
        TargetGuard* guards{nullptr};
        CommandDedup* dedup{nullptr};
        ConfirmationRegistry* confirmations{nullptr};
        QSet<ISessionScriptProvider*>* observedProviders{nullptr};
        QSet<SessionInputArbiter*>* observedArbiters{nullptr};
        QSet<SessionCommandFacade*>* observedFacades{nullptr};
        QSet<SessionCommandCoordinator*>* observedCoordinators{nullptr};
        quint64* nextCommandRequest{nullptr};
        quint64* nextScriptWrite{nullptr};
        QString instance;
        std::function<qint64()> now;
        QObject* context{nullptr};
        std::function<QString(const QString&)> clientFor;
        std::function<QByteArray(const QString&)> keyFor;
        std::function<bool(const QString&, const QJsonObject&)> send;
        std::function<void(const std::shared_ptr<Job>&, QJsonObject)> complete;
        std::function<void(const std::shared_ptr<Job>&, bool)> releaseReservation;
        std::function<void(const ScriptTaskRegistry::Task&, const ScriptWriteResult&)> recordOutcome;
        std::function<void(const CommandExecutionResult&)> finished;
        std::function<void(TerminalSession*, quint64)> cancelByUser;
        std::function<void(const ScriptWriteResult&)> finishScriptWrite;
        std::function<void()> changed;
    };

    explicit ScriptCommandUseCase(Ports ports);

    void runScript(const std::shared_ptr<Job>& job,
        const SessionDirectory::Entry& entry);
    void runCommand(const std::shared_ptr<Job>& job,
        const SessionDirectory::Entry& entry);

private:
    struct ConnectionView
    {
        QString client;
        QByteArray key;
    };
    Ports _ports;
    /// 连接 context：随用例析构自动断开，避免回调访问已析构的 _ports。
    QObject _connectionGuard;
};

} // namespace NovaTerm::Mcp
