/** @file CommandSessionAdapter.h @brief Session I/O, signed claims and MCP projection for the command runtime. */
#pragma once
#include "tasks/CommandRuntime.h"
#include "session/SessionDirectory.h"
#include <QJsonArray>
#include <QJsonObject>

namespace NovaTerm::Mcp {
class AccessStore;
struct CommandCall
{
    TaskId task;
    QString connection, id;
    ClientId client;
    QByteArray connectionKey;
    ToolRequest request;
    QByteArray confirmationDigest;
    QString requestState;
    std::optional<ConfirmationAnswer> answer;
    bool mrtr{false}, formElicitation{false};
};
class CommandSessionAdapter
{
public:
    struct Delivery
    {
        std::function<void(const TaskId&, QJsonObject)> complete;
        std::function<void(const TaskId&, std::chrono::milliseconds)> deadline;
        std::function<bool(const QString&, const QJsonObject&)> send;
        std::function<void()> changed;
    };
    CommandSessionAdapter(AccessStore&, SessionDirectory&, QString targetDirectory,
        QString instance, Delivery);
    ~CommandSessionAdapter();
    CommandSessionAdapter(const CommandSessionAdapter&) = delete;
    CommandSessionAdapter& operator=(const CommandSessionAdapter&) = delete;
    void start(CommandCall, const SessionDirectory::Entry&);
    void confirm(const QString& connection, const QString& request, ConfirmationAnswer);
    void cancel(const TaskId&);
    void expire(const TaskId&);
    void disconnect(const QString& connection);
    void revoke(const QString& client, const QString& session);
    void stop();
    void sweep();
    bool idle() const;
    bool quarantined(const QString& target) const;
    QJsonArray executionRecords() const;
    QJsonObject protectedTargets() const;
    bool acknowledgeTarget(const QString& target, const QString& expectedExecution = {});
private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace NovaTerm::Mcp
