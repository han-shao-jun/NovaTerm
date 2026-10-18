/** @file ScriptResultUseCase.cpp @brief 脚本发布结果收口实现。 */
#include "ScriptResultUseCase.h"

#include "ScriptInvocation.h"

#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include "session/ISessionScriptProvider.h"
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

ScriptResultUseCase::ScriptResultUseCase(Ports ports) : _ports(std::move(ports)) {}

void ScriptResultUseCase::recordOutcome(const ScriptTaskRegistry::Task& task,
    const ScriptWriteResult& result)
{
    if (!_ports.executions || !task.job)
        return;
    ExecutionRecord execution;
    execution.id = task.job->reservation ? task.job->reservation->executionId() : QString();
    execution.jobKey = task.job->connection + QLatin1Char('/') + task.job->id;
    execution.connection = task.job->connection;
    execution.client = task.client;
    execution.session = task.session;
    execution.epoch = task.epoch;
    execution.commandId = QStringLiteral("interactive.script");
    execution.policyVersion = task.policyVersion;
    execution.target = task.targetFingerprint;
    execution.submitted = _ports.now ? _ports.now() : 0;
    execution.complete = true;
    execution.interactive = true;
    execution.phase = TaskPhase::Final;
    const QString code = result.success ? QStringLiteral("SCRIPT_PUBLISHED_NOT_INVOKED")
        : result.errorCode.isEmpty() ? QStringLiteral("SCRIPT_WRITE_FAILED")
        : result.errorCode;
    execution.payload = failure(code, code);
    auto errorDetail = execution.payload.value("error").toObject();
    errorDetail.insert("details", QJsonObject{{"executionId", execution.id},
        {"targetPath", task.targetPath}, {"published", result.success},
        {"publicationMayHaveOccurred", result.success
            || result.errorCode == QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN")},
        {"invocationStarted", false},
        {"status", result.success ? "script_published" : "script_write_failed"}});
    execution.payload.insert("error", errorDetail);
    const QString executionId = execution.id;
    _ports.executions->insert(executionId, std::move(execution));
    if (_ports.changed)
        _ports.changed();
}

void ScriptResultUseCase::handleResult(const ScriptWriteResult& result)
{
    if (!_ports.scriptTasks || !_ports.access || !_ports.complete)
        return;
    auto taken = _ports.scriptTasks->take(result.requestId);
    if (!taken)
        return;
    const ScriptTaskRegistry::Task task = *taken;
    const auto job = task.job;
    if (!job)
        return;
    const bool uncertain = result.errorCode
        == QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN");
    const QString connection = job->connection;
    const QString jobId = job->id;
    const QString lookupKey = connection + QLatin1Char('/') + jobId;
    if (job->cancelled || !_ports.jobs->contains(lookupKey)) {
        recordOutcome(task, result);
        _ports.releaseReservation(job, !uncertain);
        return;
    }
    if (!result.success) {
        recordOutcome(task, result);
        _ports.releaseReservation(job, !uncertain);
        _ports.complete(job, failure(result.errorCode.isEmpty()
            ? QStringLiteral("SCRIPT_WRITE_FAILED") : result.errorCode,
            result.errorCode.isEmpty() ? QStringLiteral("SCRIPT_WRITE_FAILED")
            : result.errorCode));
        return;
    }
    const auto entry = _ports.sessions
        ? _ports.sessions->find(task.session)
        : std::optional<SessionDirectory::Entry>{};
    auto* coordinator = entry && entry->session
        ? entry->session->commandCoordinator() : nullptr;
    auto* facade = entry && entry->session ? entry->session->commandFacade() : nullptr;
    auto* provider = entry && entry->session ? entry->session->scriptProvider() : nullptr;
    const bool valid = entry && entry->session
        && entry->state == SessionState::Running
        && _ports.access->canRead(task.client, *entry)
        && _ports.access->canRunScriptTask(task.client, *entry)
        && entry->epoch == task.epoch
        && entry->targetFingerprint == task.targetFingerprint
        && entry->generation == task.sessionGeneration
        && coordinator && coordinator->isPromptReady()
        && coordinator->promptGeneration() == task.promptGeneration
        && facade && facade->isAvailable()
        && facade->profile().version() == task.profileVersion
        && provider && provider == task.provider.data()
        && provider->isAvailable()
        && _ports.access->version(task.client) == task.grantVersion
        && job->scriptMetadataHash == task.metadataHash
        && result.contentHash == task.contentHash
        && result.resolvedTargetPath == task.targetPath;
    if (!valid) {
        recordOutcome(task, result);
        _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
        return;
    }
    const auto command = scriptInvocationCommand(
        entry->session->runtimeConfig().transport.value(
            QStringLiteral("interactiveShellKind")).toString(),
        task.workingDirectory, task.invocation);
    if (!command || command->toUtf8().size() > 16384) {
        recordOutcome(task, result);
        _ports.complete(job, error("COMMAND_SCRIPT_INVOCATION_INVALID"));
        return;
    }
    job->resolvedCommand = *command;
    job->scriptPolicyVersion = task.policyVersion;
    job->scriptPublished = true;
    if (_ports.runCommand)
        _ports.runCommand(job, *entry);
}

} // namespace NovaTerm::Mcp
