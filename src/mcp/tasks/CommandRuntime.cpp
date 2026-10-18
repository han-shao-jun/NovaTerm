#include "CommandRuntime.h"
#include "ConfirmationRegistry.h"
#include "TargetReservation.h"
#include "mcp/CommandRiskPolicy.h"
#include "mcp/application/CommandDedup.h"
#include "mcp/contracts/Budget.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QStringDecoder>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace NovaTerm::Mcp {
namespace {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
CommandResult rejected(ErrorCode code, bool retryable = false, int delay = 0)
{
    CommandResult result;
    result.error = Error{code};
    if (retryable || delay) result.error->retry = RetryHint{retryable, std::chrono::milliseconds{delay}};
    return result;
}
void advance(TaskPhase& phase, TaskPhase next)
{
    Q_ASSERT(taskPhaseCanAdvance(phase, next));
    phase = next;
}
QString outputText(const QByteArray& bytes, qsizetype& remaining, bool& truncated)
{
    QString result;
    QStringDecoder decoder(QStringDecoder::Utf8);
    const auto append = [&](const QString& decoded) {
        QString safe;
        for (const auto& c : decoded)
            if (c == '\n' || c == '\t' || (c.unicode() >= 32
                && !(c.unicode() >= 127 && c.unicode() <= 159))) safe += c;
        const auto utf8 = safe.toUtf8();
        qsizetype end = qMin(remaining, utf8.size());
        while (end > 0 && end < utf8.size() && (quint8(utf8[end]) & 0xc0) == 0x80) --end;
        result += QString::fromUtf8(utf8.constData(), end);
        remaining -= end;
        if (end != utf8.size()) { truncated = true; return false; }
        return true;
    };
    for (qsizetype offset = 0; offset < bytes.size(); offset += 1024) {
        if (!append(decoder(QByteArrayView(bytes).sliced(offset, qMin(qsizetype(1024), bytes.size() - offset))))) return result;
        if (remaining == 0 && offset + 1024 < bytes.size()) { truncated = true; return result; }
    }
    append(decoder(QByteArrayView("\0", 1)));
    return result;
}
struct Operation
{
    CommandInvocation call;
    TaskPhase phase{TaskPhase::Preparing};
    bool cancelled{false}, confirmationAccepted{false};
    QString resolvedCommand, scriptPolicyVersion;
    std::optional<ScriptPublicationSummary> publication;
    std::unique_ptr<TargetReservation> reservation;
};
struct Execution
{
    CommandRecord record;
    QString task, client, connection, target, policy;
    quint64 request{0}, channel{0};
    qint64 submitted{0};
    std::optional<qint64> cancelDeadline;
    bool direct{false}, interactive{false};
};
struct Upload
{
    std::shared_ptr<Operation> operation;
    CommandSessionFacts facts;
    PreparedScript prepared;
    QByteArray contentHash;
    QString pathLock;
};
} // namespace

class CommandRuntime::Impl
{
public:
    Impl(CommandSessionPort& sessions, CommandTargetStore& targets, QString instance,
        Delivery delivery, std::function<qint64()> now)
        : sessions(sessions), targets(targets), instance(std::move(instance)),
          delivery(std::move(delivery)), now(std::move(now)) {}

    bool uploadPending(const std::shared_ptr<Operation>& operation) const
    {
        return std::any_of(uploads.cbegin(), uploads.cend(),
            [&operation](const Upload& upload) { return upload.operation == operation; });
    }
    void release(const std::shared_ptr<Operation>& operation, bool certain)
    {
        if (certain && operation->reservation) operation->reservation->release();
        operation->reservation.reset();
        for (auto it = paths.begin(); it != paths.end();)
            if (it.value() == operation->call.task.str()) it = paths.erase(it); else ++it;
    }
    void complete(const std::shared_ptr<Operation>& operation, std::optional<CommandResult> result)
    {
        const auto key = operation->call.task.str();
        if (!operations.contains(key)) return;
        if (operation->phase != TaskPhase::Final) advance(operation->phase, TaskPhase::Final);
        confirmations.removePending(key);
        if (result && result->error && operation->publication && !result->publication)
            result->publication = operation->publication;
        if (!uploadPending(operation)) release(operation, true);
        operations.remove(key);
        delivery.complete(operation->call.task, std::move(result));
    }
    void deadline(const std::shared_ptr<Operation>& operation, qint64 expires)
    { delivery.deadline(operation->call.task, std::chrono::milliseconds{expires - now()}); }
    std::optional<CommandSessionFacts> facts(const std::shared_ptr<Operation>& operation)
    {
        const auto reference = sessionReference(operation->call.request);
        if (!reference) { complete(operation, rejected(ErrorCode::InvalidArgument)); return {}; }
        const auto current = sessions.session(operation->call.client, reference->session);
        // A Session refresh may synchronously revoke/cancel the operation.
        if (!operations.contains(operation->call.task.str())) return {};
        if (!current || !current->readable) {
            complete(operation, rejected(operation->call.confirmationSupplied
                ? ErrorCode::CommandConfirmationStale : ErrorCode::SessionNotAvailable));
            return {};
        }
        if (current->reference.epoch != reference->epoch) {
            complete(operation, rejected(operation->call.confirmationSupplied
                ? ErrorCode::CommandConfirmationStale : ErrorCode::StaleSessionEpoch));
            return {};
        }
        return current;
    }
    ConfirmationBinding binding(const Operation& operation, const CommandSessionFacts& facts,
        const RiskAssessment& risk) const
    {
        const bool script = std::holds_alternative<RunScriptRequest>(operation.call.request);
        ConfirmationBinding result;
        result.connection = operation.call.connection; result.client = operation.call.client;
        result.session = facts.reference.session; result.epoch = facts.reference.epoch;
        result.targetFingerprint = facts.target; result.policyVersion = risk.policyVersion;
        result.profileVersion = facts.profileVersion;
        result.grantVersion = ClientAccessVersion{facts.grantVersion};
        result.promptGeneration = PromptRevision{facts.promptGeneration};
        result.tool = script ? ConfirmationTool::Script : ConfirmationTool::Command;
        result.decision = risk.decision == RiskDecision::Unknown ? ConfirmationDecision::Unknown : ConfirmationDecision::Confirm;
        result.payloadHash = script ? operation.call.confirmationDigest
            : QCryptographicHash::hash(std::get<RunCommandRequest>(operation.call.request).command.toUtf8(), QCryptographicHash::Sha256);
        if (!script) result.userInputGeneration = InputRevision{facts.userInputGeneration};
        return result;
    }
    bool awaitConfirmation(const std::shared_ptr<Operation>& operation,
        const CommandSessionFacts& facts, const RiskAssessment& risk,
        const PreparedScript& prepared = {})
    {
        if (operation->confirmationAccepted) return true;
        const auto current = binding(*operation, facts, risk);
        if (operation->call.confirmationSupplied) {
            if (!operation->call.detachedConfirmation || !operation->call.confirmation
                || !confirmations.consume(*operation->call.confirmation, current, now())) {
                complete(operation, rejected(ErrorCode::CommandConfirmationStale)); return false;
            }
            const auto answer = operation->call.answer.value_or(ConfirmationAnswer::Decline);
            if (answer != ConfirmationAnswer::Accept) {
                complete(operation, rejected(answer == ConfirmationAnswer::Cancel
                    ? ErrorCode::CommandConfirmationCancelled : ErrorCode::CommandConfirmationDeclined)); return false;
            }
            operation->confirmationAccepted = true;
            operation->call.confirmationSupplied = false;
            operation->call.confirmation.reset(); operation->call.answer.reset();
            return true;
        }
        if (!operation->call.formElicitation) {
            complete(operation, rejected(ErrorCode::ClientConfirmationUnavailable)); return false;
        }
        const auto peer = operation->call.connection.str();
        const bool pending = operation->call.detachedConfirmation
            ? confirmations.hasActiveForConnection(peer) : confirmations.hasPendingForConnection(peer);
        if (pending) { complete(operation, rejected(ErrorCode::Busy, true, 250)); return false; }
        ConfirmationTransaction transaction{current, newId(), now() + Budget::ConfirmationTimeout.count()};
        const auto key = operation->call.task.str();
        if (operation->call.detachedConfirmation) confirmations.registerActive(transaction);
        else confirmations.insertPending(key, PendingConfirmationRecord{key, {}, transaction});
        CommandPrompt prompt{transaction, operation->call.request, prepared.targetPath,
            risk.reasons, prepared.targetExistsLocally, operation->call.detachedConfirmation};
        if (!delivery.confirm(operation->call.task, prompt)) {
            confirmations.clearActive(peer, transaction.nonce); confirmations.removePending(key);
            complete(operation, rejected(ErrorCode::ClientConfirmationUnavailable)); return false;
        }
        if (operation->call.detachedConfirmation) complete(operation, std::nullopt);
        else deadline(operation, transaction.expires);
        return false;
    }
    int executionCount(const QString& client) const
    {
        return int(std::count_if(executions.cbegin(), executions.cend(),
            [&client](const Execution& execution) { return execution.client == client; }));
    }
    Execution* executionByRequest(quint64 request)
    {
        for (auto& execution : executions) if (execution.request == request) return &execution;
        return nullptr;
    }
    bool reserve(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts,
        const QString& execution)
    {
        Error error;
        auto reservation = TargetReservation::acquire(targets, facts.target, execution, instance, error);
        if (!reservation) { CommandResult result; result.error = error; complete(operation, result); return false; }
        operation->reservation = std::move(reservation);
        advance(operation->phase, TaskPhase::Reserved);
        return true;
    }
    bool capacity(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts)
    {
        if (executionCount(operation->call.client.str()) >= Budget::MaxExecutionRecordsPerClient
            || executions.size() + uploads.size() >= Budget::MaxExecutionRecords || dedup.blocked(facts.target)) {
            complete(operation, rejected(ErrorCode::Busy, true, 5000)); return false;
        }
        return true;
    }
    bool quarantined(const QString& target) const
    {
        for (const auto& marker : targets.protections()) if (marker.target == target) return true;
        return false;
    }
    void submit(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts,
        const QString& executionId, const QByteArray& command, const QString& commandId,
        const QString& policy, bool direct, bool interactive, std::chrono::milliseconds timeout)
    {
        advance(operation->phase, TaskPhase::Submitted);
        const auto requestId = nextCommand++;
        Execution execution;
        execution.record = {executionId, commandId, facts.reference, TaskPhase::Submitted, false, {}};
        execution.task = operation->call.task.str(); execution.client = operation->call.client.str();
        execution.connection = operation->call.connection.str(); execution.target = facts.target;
        execution.channel = direct ? facts.coordinator : facts.executor; execution.direct = direct; execution.policy = policy;
        execution.request = requestId; execution.submitted = now(); execution.interactive = interactive;
        executions.insert(executionId, execution);
        operation->reservation.reset(); // Execution now owns the persistent protection, including unknown outcomes.
        CommandExecutionRequest request{requestId, command, {Budget::CommandOutputBytes, int(timeout.count())}, commandId};
        if (interactive || facts.mode == CommandExecutionMode::InteractiveFramed) {
            request.executionNonce = newId().toUtf8();
            request.expectedPromptGeneration = facts.promptFree ? 0 : facts.promptGeneration;
        }
        if (!sessions.submit(execution.channel, direct, request)) {
            executions.remove(executionId); targets.release(facts.target, executionId);
            complete(operation, rejected(interactive ? ErrorCode::SessionCommandBusy : ErrorCode::Busy, !interactive, interactive ? 0 : 250));
            return;
        }
        operation->publication.reset();
        if (operation->phase == TaskPhase::Submitted) advance(operation->phase, TaskPhase::Observing);
        if (auto it = executions.find(executionId); it != executions.end() && it->record.phase == TaskPhase::Submitted)
            advance(it->record.phase, TaskPhase::Observing);
        // Synchronous completion is valid; it must not resurrect a final operation.
        dedup.remember(facts.target, now());
        deadline(operation, now() + timeout.count()); delivery.changed();

    }
    void run(const std::shared_ptr<Operation>& operation);
    void runScript(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts);
    void runCommand(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts);
    void runDiagnostic(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts);
    void finishExecution(const CommandExecutionResult&);
    void finishPublication(const ScriptPublicationResult&);
    void recordPublication(const Upload&, const ScriptPublicationResult&);
    void cancel(const TaskId&);

    CommandSessionPort& sessions;
    CommandTargetStore& targets;
    QString instance;
    Delivery delivery;
    std::function<qint64()> now;
    ConfirmationRegistry confirmations;
    CommandDedup dedup;
    QHash<QString, std::shared_ptr<Operation>> operations;
    QHash<QString, Execution> executions;
    QHash<quint64, Upload> uploads;
    QHash<QString, QString> paths;
    quint64 nextCommand{quint64(1) << 60}, nextPublication{quint64(1) << 59};
};

void CommandRuntime::Impl::run(const std::shared_ptr<Operation>& operation)
{
    const auto current = facts(operation);
    if (!current) return;
    if (std::holds_alternative<ExecuteCommandRequest>(operation->call.request)) runDiagnostic(operation, *current);
    else if (std::holds_alternative<RunScriptRequest>(operation->call.request)) runScript(operation, *current);
    else if (std::holds_alternative<RunCommandRequest>(operation->call.request)) runCommand(operation, *current);
    else complete(operation, rejected(ErrorCode::InvalidArgument));
}
void CommandRuntime::Impl::runDiagnostic(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts)
{
    const auto& request = std::get<ExecuteCommandRequest>(operation->call.request);
    const auto id = request.command.str();
    if (!facts.diagnostics.contains(id)) { complete(operation, rejected(ErrorCode::CommandNotAllowed)); return; }
    if (!facts.allowedDiagnostics.contains(id)) { complete(operation, rejected(ErrorCode::CommandPermissionRequired)); return; }
    const auto& ticket = operation->call.ticket;
    if (!ticket || ticket->session.session != facts.reference.session || ticket->session.epoch != facts.reference.epoch
        || ticket->command != request.command || ticket->policyVersion != request.policyVersion
        || ticket->profileVersion != facts.profileVersion || ticket->execution.isEmpty()) {
        complete(operation, rejected(ErrorCode::CommandTicketScopeInvalid)); return;
    }
    if (const auto previous = executions.constFind(ticket->execution); previous != executions.cend()) {
        if (previous->connection != operation->call.connection.str()) complete(operation, rejected(ErrorCode::CommandTicketScopeInvalid));
        else complete(operation, previous->record.complete ? previous->record.result : rejected(ErrorCode::CommandInProgress, true, 250));
        return;
    }
    if (ticket->expires <= now()) { complete(operation, rejected(ErrorCode::CommandTicketExpired)); return; }
    if (ticket->grantVersion.value() != facts.grantVersion) { complete(operation, rejected(ErrorCode::CommandPermissionRequired)); return; }
    if (ticket->policyVersion != facts.diagnosticPolicyVersion) { complete(operation, rejected(ErrorCode::CommandPolicyChanged)); return; }
    if (request.hasDiagnosticArguments) { complete(operation, rejected(ErrorCode::CommandNotAllowed)); return; }
    if (!facts.executorAvailable || facts.target.isEmpty()) {
        complete(operation, rejected(facts.profileAvailable ? ErrorCode::CommandUnavailable : ErrorCode::CommandProfileUnavailable)); return;
    }
    if (!facts.running) { complete(operation, rejected(ErrorCode::SessionNotReady)); return; }
    if (quarantined(facts.target)) { complete(operation, rejected(ErrorCode::CommandExecutionQuarantined)); return; }
    if (!capacity(operation, facts) || !reserve(operation, facts, ticket->execution)) return;
    submit(operation, facts, ticket->execution, facts.diagnostics.value(id), id,
        facts.diagnosticPolicyVersion, false, false, Budget::CommandTimeout);
}
void CommandRuntime::Impl::runCommand(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts)
{
    const auto* command = std::get_if<RunCommandRequest>(&operation->call.request);
    if (command && !facts.commandAllowed) {
        complete(operation, rejected(operation->call.confirmationSupplied ? ErrorCode::CommandConfirmationStale : ErrorCode::CommandPermissionRequired)); return;
    }
    if (!facts.running) { complete(operation, rejected(ErrorCode::SessionNotReady)); return; }
    const auto text = command ? command->command : operation->resolvedCommand;
    const auto risk = CommandRiskPolicy{}.classify(text);
    if (risk.decision == RiskDecision::Deny) { complete(operation, rejected(ErrorCode::CommandNotAllowed)); return; }
    if (risk.decision == RiskDecision::Allow && operation->call.confirmationSupplied) {
        complete(operation, rejected(ErrorCode::CommandConfirmationStale)); return;
    }
    if (risk.decision != RiskDecision::Allow && !operation->confirmationAccepted
        && !operation->call.formElicitation && !operation->call.confirmationSupplied) {
        complete(operation, rejected(ErrorCode::ClientConfirmationUnavailable)); return;
    }
    if (!facts.coordinatorAvailable || (!facts.trustedProfile && !facts.promptFree)) {
        complete(operation, rejected(ErrorCode::CommandProfileUnavailable)); return;
    }
    if (!facts.promptFree && !facts.promptReady) { complete(operation, rejected(ErrorCode::SessionCommandNotReady)); return; }
    if (risk.decision != RiskDecision::Allow && !awaitConfirmation(operation, facts, risk)) return;
    if (!facts.executorAvailable || facts.target.isEmpty()) { complete(operation, rejected(ErrorCode::CommandUnavailable)); return; }
    bool ownsReservation = false;
    if (operation->reservation && operation->reservation->fingerprint() == facts.target)
        for (const auto& marker : targets.protections())
            if (marker.target == facts.target && marker.execution == operation->reservation->executionId()) ownsReservation = true;
    if (operation->reservation && !ownsReservation) { complete(operation, rejected(ErrorCode::CommandConfirmationStale)); return; }
    if (quarantined(facts.target) && !ownsReservation) { complete(operation, rejected(ErrorCode::CommandExecutionQuarantined)); return; }
    if (!capacity(operation, facts)) return;
    const auto execution = ownsReservation ? operation->reservation->executionId() : newId();
    if (!ownsReservation && !reserve(operation, facts, execution)) return;
    const bool direct = facts.promptFree || (facts.kind == CommandSessionKind::Local && facts.mode == CommandExecutionMode::Isolated);
    const auto timeout = command ? command->timeout : std::get<RunScriptRequest>(operation->call.request).timeout;
    const bool script = !command;
    submit(operation, facts, execution, text.toUtf8(), script ? "interactive.script" : "interactive.free",
        script ? operation->scriptPolicyVersion : risk.policyVersion, direct, true, timeout);
}
void CommandRuntime::Impl::runScript(const std::shared_ptr<Operation>& operation, const CommandSessionFacts& facts)
{
    if (!facts.scriptAllowed) {
        complete(operation, rejected(operation->call.confirmationSupplied ? ErrorCode::CommandConfirmationStale : ErrorCode::ScriptPermissionRequired)); return;
    }
    if (!facts.running) { complete(operation, rejected(ErrorCode::SessionNotReady)); return; }
    if (!facts.providerAvailable) { complete(operation, rejected(ErrorCode::ScriptProviderUnavailable)); return; }
    const bool isolatedLocal = facts.kind == CommandSessionKind::Local && facts.mode == CommandExecutionMode::Isolated;
    if (!facts.coordinatorAvailable || !facts.trustedProfile || !facts.executorAvailable
        || (facts.mode != CommandExecutionMode::InteractiveFramed && !isolatedLocal)) {
        complete(operation, rejected(ErrorCode::CommandProfileUnavailable)); return;
    }
    const auto& request = std::get<RunScriptRequest>(operation->call.request);
    const auto content = request.scriptContent.toUtf8();
    const auto risk = CommandRiskPolicy{}.classifyScript(content, request.targetPath, request.workingDirectory, request.invocation);
    if (risk.decision != RiskDecision::Confirm) {
        complete(operation, rejected(risk.decision == RiskDecision::Deny ? ErrorCode::CommandNotAllowed : ErrorCode::CommandScriptRiskUnknown)); return;
    }
    const auto preparedResult = sessions.prepareScript(facts, request);
    if (const auto* error = errorIfAny(preparedResult)) { CommandResult result; result.error = *error; complete(operation, result); return; }
    const auto prepared = std::get<PreparedScript>(preparedResult);
    if (!facts.promptReady) { complete(operation, rejected(ErrorCode::SessionCommandNotReady)); return; }
    if (!awaitConfirmation(operation, facts, risk, prepared)) return;
    if (quarantined(facts.target)) { complete(operation, rejected(ErrorCode::CommandExecutionQuarantined)); return; }
    if (dedup.blocked(facts.target) || uploads.size() >= Budget::MaxPendingScriptWrites
        || executions.size() >= Budget::MaxExecutionRecords) { complete(operation, rejected(ErrorCode::Busy, true, 5000)); return; }
    const auto pathLock = facts.target + '/' + prepared.targetPath;
    if (paths.contains(pathLock)) { complete(operation, rejected(ErrorCode::Busy, true, 500)); return; }
    if (prepared.command.isEmpty() || prepared.command.toUtf8().size() > 16384) {
        complete(operation, rejected(ErrorCode::CommandScriptInvocationInvalid)); return;
    }
    if (!reserve(operation, facts, newId())) return;
    const auto requestId = nextPublication++;
    uploads.insert(requestId, Upload{operation, facts, prepared,
        QCryptographicHash::hash(content, QCryptographicHash::Sha256), pathLock});
    paths.insert(pathLock, operation->call.task.str());
    operation->scriptPolicyVersion = risk.policyVersion;
    deadline(operation, now() + Budget::ScriptWriteTimeout.count());
    if (!sessions.publish(facts.provider, ScriptPublicationRequest{requestId, content, request.targetPath, request.workingDirectory})) {
        uploads.remove(requestId); complete(operation, rejected(ErrorCode::ScriptWriteFailed));
    }
}
void CommandRuntime::Impl::recordPublication(const Upload& upload, const ScriptPublicationResult& result)
{
    const auto& operation = upload.operation;
    const auto executionId = operation->reservation ? operation->reservation->executionId() : QString{};
    CommandResult outcome = rejected(result.success ? ErrorCode::ScriptPublishedNotInvoked : result.error.value_or(ErrorCode::ScriptWriteFailed));
    outcome.publication = ScriptPublicationSummary{executionId, upload.prepared.targetPath,
        result.success ? "script_published" : "script_write_failed", result.success,
        result.success || result.error == ErrorCode::ScriptPublicationOutcomeUnknown, false};
    Execution execution;
    execution.record = {executionId, "interactive.script", upload.facts.reference, TaskPhase::Final, true, outcome};
    execution.client = operation->call.client.str(); execution.connection = operation->call.connection.str();
    execution.target = upload.facts.target; execution.submitted = now();
    executions.insert(executionId, execution);
    operation->publication = outcome.publication;
    delivery.changed();
}
void CommandRuntime::Impl::finishPublication(const ScriptPublicationResult& result)
{
    auto it = uploads.find(result.requestId);
    if (it == uploads.end()) return;
    const auto upload = it.value(); uploads.erase(it);
    const auto operation = upload.operation;
    const bool uncertain = result.error == ErrorCode::ScriptPublicationOutcomeUnknown;
    if (operation->cancelled || !operations.contains(operation->call.task.str())) {
        recordPublication(upload, result); release(operation, !uncertain); return;
    }
    if (!result.success) {
        recordPublication(upload, result); release(operation, !uncertain);
        complete(operation, rejected(result.error.value_or(ErrorCode::ScriptWriteFailed))); return;
    }
    const auto current = facts(operation);
    if (!current) { recordPublication(upload, result); release(operation, true); return; }
    const bool valid = current->running && current->scriptAllowed && current->reference.epoch == upload.facts.reference.epoch
        && current->target == upload.facts.target && current->sessionGeneration == upload.facts.sessionGeneration
        && current->promptReady && current->promptGeneration == upload.facts.promptGeneration
        && current->executorAvailable && current->profileVersion == upload.facts.profileVersion
        && current->providerAvailable && current->provider == upload.facts.provider
        && current->grantVersion == upload.facts.grantVersion
        && result.contentHash == upload.contentHash && result.resolvedTargetPath == upload.prepared.targetPath;
    if (!valid) {
        recordPublication(upload, result); complete(operation, rejected(ErrorCode::CommandConfirmationStale)); return;
    }
    operation->resolvedCommand = upload.prepared.command;
    operation->publication = ScriptPublicationSummary{operation->reservation->executionId(), upload.prepared.targetPath,
        "script_published", true, true, false};
    runCommand(operation, *current);
}
void CommandRuntime::Impl::finishExecution(const CommandExecutionResult& result)
{
    auto* execution = executionByRequest(result.requestId);
    if (!execution) return;
    if (execution->record.complete) {
        if (!result.executionMayHaveStarted || result.terminationConfirmed) { targets.release(execution->target, execution->record.executionId); delivery.changed(); }
        return;
    }
    auto& record = execution->record;
    record.complete = true; advance(record.phase, TaskPhase::Final);
    CommandExecutionSummary summary;
    summary.executionId = record.executionId; summary.instanceId = instance;
    summary.commandId = record.commandId; summary.session = record.session;
    summary.policyVersion = execution->policy;
    summary.outputTruncated = result.outputTruncated;
    qsizetype remaining = Budget::CommandOutputBytes;
    summary.standardOutput = outputText(result.standardOutput, remaining, summary.outputTruncated);
    summary.standardError = outputText(result.standardError, remaining, summary.outputTruncated);
    summary.executionMayHaveStarted = result.executionMayHaveStarted; summary.terminationConfirmed = result.terminationConfirmed;
    summary.exitCode = result.exitCode; if (result.terminationConfirmed) summary.finishedAt = utcNow();
    const bool completed = result.outcome == CommandExecutionOutcome::Completed && !summary.outputTruncated
        && result.terminationConfirmed && (result.exitCode ? *result.exitCode == 0 : execution->interactive);
    summary.status = completed ? "completed" : "failed";
    ErrorCode code = ErrorCode::CommandFailed;
    if (summary.outputTruncated) code = ErrorCode::CommandOutputLimit;
    else if (result.outcome == CommandExecutionOutcome::TimedOut) { code = ErrorCode::CommandTimeout; summary.status = "timed_out"; }
    else if (result.outcome == CommandExecutionOutcome::Cancelled) { code = ErrorCode::CommandOutcomeUnknown; summary.status = "cancelled"; }
    else if (result.executionMayHaveStarted && !result.terminationConfirmed) { code = ErrorCode::CommandOutcomeUnknown; summary.status = "unknown"; }
    record.result = completed ? CommandResult{} : rejected(code);
    record.result.execution = summary;
    const auto outcome = record.result;
    const auto task = execution->task; execution->task.clear();
    if (!result.executionMayHaveStarted || result.terminationConfirmed) targets.release(execution->target, record.executionId);
    if (const auto operation = operations.value(task)) complete(operation, outcome);
    delivery.changed();
}
void CommandRuntime::Impl::cancel(const TaskId& id)
{
    const auto operation = operations.value(id.str());
    if (!operation || operation->cancelled) return;
    operation->cancelled = true;
    for (const auto request : uploads.keys()) {
        const auto upload = uploads.value(request);
        if (upload.operation == operation) sessions.cancelPublication(upload.facts.provider, request);
    }
    for (const auto& snapshot : executions.values()) {
        if (snapshot.record.complete || snapshot.task != id.str()) continue;
        if (auto* current = executionByRequest(snapshot.request)) {
            current->task.clear(); current->cancelDeadline = now() + Budget::CancelCompletionGrace.count();
        }
        sessions.cancel(snapshot.channel, snapshot.direct, snapshot.request, false);
    }
    confirmations.removePending(id.str());
    if (!uploadPending(operation)) release(operation, true);
    operations.remove(id.str());
}

CommandRuntime::CommandRuntime(CommandSessionPort& sessions, CommandTargetStore& targets,
    QString instance, Delivery delivery, std::function<qint64()> now)
    : _impl(std::make_unique<Impl>(sessions, targets, std::move(instance), std::move(delivery), std::move(now))) {}
CommandRuntime::~CommandRuntime()
{
    // 停止后端但不向正在析构的宿主交付结果；未知副作用仍保留保护标记。
    _impl->delivery = Delivery{
        [](const TaskId&, std::optional<CommandResult>) {},
        [](const TaskId&, std::chrono::milliseconds) {},
        [](const TaskId&, const CommandPrompt&) { return false; }, [] {}};
    stop();
}
void CommandRuntime::start(CommandInvocation call)
{
    auto operation = std::make_shared<Operation>(); operation->call = std::move(call);
    _impl->operations.insert(operation->call.task.str(), operation); _impl->run(operation);
}
void CommandRuntime::confirm(const TaskId& task, ConfirmationAnswer answer)
{
    const auto pending = _impl->confirmations.takePending(task.str());
    if (!pending) return;
    const auto operation = _impl->operations.value(task.str());
    if (!operation) return;
    if (answer != ConfirmationAnswer::Accept) {
        _impl->complete(operation, rejected(answer == ConfirmationAnswer::Cancel
            ? ErrorCode::CommandConfirmationCancelled : ErrorCode::CommandConfirmationDeclined)); return;
    }
    const auto current = _impl->facts(operation);
    if (!current) return;
    const bool script = std::holds_alternative<RunScriptRequest>(operation->call.request);
    RiskAssessment risk;
    if (script) {
        const auto& request = std::get<RunScriptRequest>(operation->call.request);
        risk = CommandRiskPolicy{}.classifyScript(request.scriptContent.toUtf8(), request.targetPath, request.workingDirectory, request.invocation);
    } else risk = CommandRiskPolicy{}.classify(std::get<RunCommandRequest>(operation->call.request).command);
    if (!(script ? current->scriptAllowed : current->commandAllowed) || !current->coordinatorAvailable
        || !current->executor || (script && !current->providerAvailable)
        || (!current->promptFree && !current->promptReady)
        || (risk.decision != RiskDecision::Confirm && risk.decision != RiskDecision::Unknown)
        || !ConfirmationRegistry::validate(pending->transaction, _impl->binding(*operation, *current, risk), _impl->now())) {
        _impl->complete(operation, rejected(ErrorCode::CommandConfirmationStale)); return;
    }
    operation->confirmationAccepted = true;
    _impl->deadline(operation, _impl->now() + Budget::ConfirmationTimeout.count());
    if (script) _impl->runScript(operation, *current); else _impl->runCommand(operation, *current);
}
void CommandRuntime::executionFinished(const CommandExecutionResult& result) { _impl->finishExecution(result); }
void CommandRuntime::publicationFinished(const ScriptPublicationResult& result) { _impl->finishPublication(result); }
void CommandRuntime::providerUnavailable(quint64 provider)
{
    for (const auto request : _impl->uploads.keys()) {
        const auto upload = _impl->uploads.value(request);
        if (upload.facts.provider != provider) continue;
        _impl->uploads.remove(request);
        ScriptPublicationResult unknown; unknown.requestId = request; unknown.error = ErrorCode::ScriptPublicationOutcomeUnknown;
        _impl->recordPublication(upload, unknown); _impl->release(upload.operation, false);
        _impl->complete(upload.operation, rejected(ErrorCode::ScriptProviderUnavailable));
    }
}
void CommandRuntime::userInput(const SessionId& session, quint64 generation)
{
    for (const auto& upload : _impl->uploads.values())
        if (upload.facts.reference.session == session && upload.facts.userInputGeneration != generation) {
            const auto task = upload.operation->call.task;
            _impl->cancel(task); _impl->delivery.complete(task, rejected(ErrorCode::ScriptCancelledByUser));
        }
}
void CommandRuntime::cancel(const TaskId& task) { _impl->cancel(task); }
void CommandRuntime::expire(const TaskId& task)
{
    for (const auto& execution : _impl->executions.values()) {
        if (execution.record.complete || execution.task != task.str()) continue;
        _impl->sessions.cancel(execution.channel, execution.direct, execution.request, true);
        CommandExecutionResult timedOut; timedOut.requestId = execution.request;
        timedOut.executionMayHaveStarted = true; timedOut.outcome = CommandExecutionOutcome::TimedOut;
        _impl->finishExecution(timedOut); return;
    }
    if (_impl->operations.contains(task.str())) {
        _impl->cancel(task); _impl->delivery.complete(task, rejected(ErrorCode::DeadlineExceeded));
    }
}
void CommandRuntime::disconnect(const QString& connection)
{
    static_cast<void>(_impl->confirmations.takeConnection(connection));
    for (const auto& operation : _impl->operations.values())
        if (operation->call.connection.str() == connection)
            _impl->cancel(operation->call.task);
}
void CommandRuntime::revoke(const QString& client, const QString& session)
{
    _impl->confirmations.removeScope(client, session);
    for (const auto& operation : _impl->operations.values()) {
        const auto reference = sessionReference(operation->call.request);
        if ((!client.isEmpty() && operation->call.client.str() != client)
            || (!session.isEmpty() && (!reference || reference->session.str() != session))) continue;
        const auto task = operation->call.task;
        _impl->cancel(task);
        _impl->delivery.complete(task, rejected(ErrorCode::SessionNotAvailable));
    }
}
void CommandRuntime::stop()
{
    for (const auto& task : _impl->operations.keys()) _impl->cancel(TaskId{task});
    for (const auto& execution : _impl->executions.values()) {
        if (execution.record.complete) continue;
        CommandExecutionResult result; result.requestId = execution.request;
        result.executionMayHaveStarted = true; result.outcome = CommandExecutionOutcome::Disconnected;
        _impl->finishExecution(result);
    }
}
void CommandRuntime::sweep()
{
    const auto now = _impl->now();
    for (const auto& expired : _impl->confirmations.sweep(now))
        if (const auto operation = _impl->operations.value(expired.jobKey))
            _impl->complete(operation, rejected(ErrorCode::CommandConfirmationExpired));
    for (const auto& execution : _impl->executions.values())
        if (!execution.record.complete && execution.cancelDeadline && *execution.cancelDeadline <= now) {
            CommandExecutionResult result; result.requestId = execution.request;
            result.executionMayHaveStarted = true; result.outcome = CommandExecutionOutcome::Cancelled;
            _impl->finishExecution(result);
        }
    for (auto it = _impl->executions.begin(); it != _impl->executions.end();)
        if (it->record.complete && now - it->submitted >= Budget::ExecutionRecordRetention.count()) it = _impl->executions.erase(it); else ++it;
    _impl->dedup.sweep(now);
}
bool CommandRuntime::idle() const
{ return _impl->operations.empty() && _impl->uploads.empty() && _impl->executions.empty() && _impl->dedup.empty() && _impl->confirmations.empty(); }
bool CommandRuntime::quarantined(const QString& target) const { return _impl->quarantined(target); }
QList<CommandRecord> CommandRuntime::executionRecords() const
{ QList<CommandRecord> records; for (const auto& execution : _impl->executions) records.append(execution.record); return records; }
QList<TargetProtection> CommandRuntime::protectedTargets() const { return _impl->targets.protections(); }
bool CommandRuntime::acknowledgeTarget(const QString& target, const QString& expected)
{
    for (const auto& upload : _impl->uploads) if (upload.facts.target == target) return false;
    for (const auto& execution : _impl->executions) if (!execution.record.complete && execution.target == target) return false;
    const bool released = expected.isEmpty() ? _impl->targets.acknowledge(target) : _impl->targets.release(target, expected);
    if (released) _impl->delivery.changed();
    return released;
}
} // namespace NovaTerm::Mcp
