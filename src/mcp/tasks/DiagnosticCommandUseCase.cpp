/** @file DiagnosticCommandUseCase.cpp @brief 固定诊断命令用例实现。 */
#include "DiagnosticCommandUseCase.h"

#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include "mcp/contracts/Budget.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionCommandFacade.h"

namespace NovaTerm::Mcp {
namespace {
QJsonObject error(const char* code, bool retryable = false, int retryAfter = 0)
{
    return failure(QString::fromLatin1(code), QString::fromLatin1(code),
        retryable, retryAfter);
}
} // namespace

DiagnosticCommandUseCase::DiagnosticCommandUseCase(Ports ports)
    : _ports(std::move(ports)) {}

void DiagnosticCommandUseCase::run(const std::shared_ptr<Job>& job,
    const SessionDirectory::Entry& entry, const QString& client,
    const QByteArray& connectionKey)
{
    auto* access = _ports.access;
    auto* executions = _ports.executions;
    auto* scriptTasks = _ports.scriptTasks;
    auto* guards = _ports.guards;
    auto* dedup = _ports.dedup;
    const auto complete = _ports.complete;
    if (!access || !executions || !scriptTasks || !guards || !dedup || !complete)
        return;
    const auto& call = std::get<ExecuteCommandRequest>(job->request);
    const auto commandId = call.command.str();
    auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
    const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
    const auto definition = CommandPolicy::find(commandId, profile);
    if (!definition) { complete(job, error("COMMAND_NOT_ALLOWED")); return; }
    if (!access->commands(client, entry).contains(commandId)) {
        complete(job, error("COMMAND_PERMISSION_REQUIRED")); return;
    }
    const auto ticket = verifyToken(call.commandTicket, connectionKey);
    if (!ticket || ticket->value("kind") != "command" || ticket->value("session") != entry.id
        || ticket->value("epoch") != entry.epoch || ticket->value("command") != commandId
        || ticket->value("policy") != call.policyVersion
        || ticket->value("profile") != profile.version()) {
        complete(job, error("COMMAND_TICKET_SCOPE_INVALID")); return;
    }
    const QString executionId = ticket->value("execution").toString();
    if (const auto* previous = executions->find(executionId)) {
        if (previous->connection != job->connection) {
            complete(job, error("COMMAND_TICKET_SCOPE_INVALID")); return;
        }
        complete(job, previous->complete ? previous->payload
            : error("COMMAND_IN_PROGRESS", true, 250));
        return;
    }
    const auto now = _ports.now ? _ports.now() : 0;
    if (ticket->value("expires").toDouble() <= now) {
        complete(job, error("COMMAND_TICKET_EXPIRED")); return;
    }
    if (ticket->value("grant").toString()
        != QString::number(access->version(client))) {
        complete(job, error("COMMAND_PERMISSION_REQUIRED")); return;
    }
    const QString policyVersion = CommandPolicy::version(profile);
    if (ticket->value("policy") != policyVersion) {
        complete(job, error("COMMAND_POLICY_CHANGED")); return;
    }
    if (call.hasDiagnosticArguments) { complete(job, error("COMMAND_NOT_ALLOWED")); return; }
    if (!facade || !facade->isAvailable() || entry.targetFingerprint.isEmpty()) {
        complete(job, error(profile.isAvailable()
            ? "COMMAND_UNAVAILABLE" : "COMMAND_PROFILE_UNAVAILABLE")); return;
    }
    if (entry.state != SessionState::Running) {
        complete(job, error("SESSION_NOT_READY")); return;
    }
    const int count = executions->countForClient(client);
    if (guards->markers().contains(entry.targetFingerprint)) {
        complete(job, error("COMMAND_EXECUTION_QUARANTINED")); return;
    }
    if (count >= Budget::MaxExecutionRecordsPerClient
        || executions->size() + scriptTasks->size() >= Budget::MaxExecutionRecords
        || dedup->blocked(entry.targetFingerprint)) {
        complete(job, error("BUSY", true, 5000)); return;
    }
    QString rejected;
    if (!guards->reserve(entry.targetFingerprint, executionId,
            _ports.instance, rejected)) {
        complete(job, failure(rejected, rejected, rejected == "BUSY", 250)); return;
    }
    auto* observed = _ports.observedFacades;
    if (observed && !observed->contains(facade)) {
        observed->insert(facade);
        QObject::connect(facade, &SessionCommandFacade::finished, _ports.context,
            [finished = _ports.finished](const CommandExecutionResult& result) {
                if (finished) finished(result);
            });
        QObject::connect(facade, &QObject::destroyed, _ports.context,
            [observed, facade] { observed->remove(facade); });
    }
    const quint64 request = (*_ports.nextRequest)++;
    ExecutionRecord execution;
    execution.id = executionId;
    execution.connection = job->connection;
    execution.client = client;
    execution.session = entry.id;
    execution.epoch = entry.epoch;
    execution.commandId = commandId;
    execution.policyVersion = policyVersion;
    execution.target = entry.targetFingerprint;
    execution.executorRequestId = request;
    execution.submitted = now;
    execution.facade = facade;
    execution.jobKey = job->connection + QLatin1Char('/') + job->id;
    execution.phase = TaskPhase::Submitted;
    executions->insert(executionId, execution);
    CommandExecutionRequest executionRequest{
        request, definition->command, {CommandOutputBytes, int(Budget::CommandTimeout.count())}, commandId, {}, 0};
    if (facade->capabilities().mode == CommandExecutionMode::InteractiveFramed
        && entry.session && entry.session->commandCoordinator()) {
        executionRequest.executionNonce = newId().toUtf8();
        executionRequest.expectedPromptGeneration =
            entry.session->commandCoordinator()->promptGeneration();
    }
    if (!facade->execute(executionRequest)) {
        executions->remove(executionId);
        guards->release(entry.targetFingerprint, executionId);
        complete(job, error("BUSY", true, 250)); return;
    }
    dedup->remember(entry.targetFingerprint, now);
    job->deadline = now + Budget::CommandTimeout.count();
    if (_ports.changed) _ports.changed();
}

} // namespace NovaTerm::Mcp
