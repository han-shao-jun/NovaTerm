#include "CommandSessionAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "CommandPolicy.h"
#include "protocol/ToolCodec.h"
#include "session/ISessionScriptProvider.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionInputArbiter.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QSet>

namespace NovaTerm::Mcp {
namespace {
QString safeScriptPreview(const QByteArray& content, qsizetype maximumBytes)
{
    const QString decoded = QString::fromUtf8(content.left(maximumBytes));
    QString preview;
    preview.reserve(decoded.size());
    for (const auto& character : decoded) {
        if (character == QLatin1Char('\n') || character == QLatin1Char('\t')) {
            preview.append(character);
        } else if (character == QLatin1Char('\r')) {
            preview.append(QStringLiteral("\\r"));
        } else if (character.category() == QChar::Other_Control) {
            preview.append(QStringLiteral("\\u%1").arg(
                character.unicode(), 4, 16, QLatin1Char('0')));
        } else {
            preview.append(character);
        }
    }
    return preview;
}

QString scriptConfirmationMessage(const QByteArray& content,
    const QString& targetPath, const QString& workingDirectory,
    const QString& invocation, const QStringList& reasons,
    bool targetExistsLocally)
{
    constexpr qsizetype PreviewBytes = 4096;
    const qsizetype previewLength = (std::min)(content.size(), PreviewBytes);
    QString preview = safeScriptPreview(content, previewLength);
    if (previewLength < content.size())
        preview += QStringLiteral("\n… preview truncated …");
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    const QString overwrite = targetExistsLocally
        ? QStringLiteral("The existing local target will be overwritten after approval.")
        : QStringLiteral("The requested target may exist and will be overwritten after approval.");
    return QStringLiteral(
        "Confirm writing and running this script on the selected host.\n\n"
        "Target: %1\nWorking directory: %2\nInvocation shown in terminal: %3\n"
        "Size: %4 bytes\nSHA-256: %5\nRisk: %6\n%7 "
        "The file is not automatically deleted. Script contents are not sent to the terminal.\n\n"
        "Script preview:\n%8")
        .arg(targetPath).arg(workingDirectory).arg(invocation).arg(content.size())
        .arg(hash).arg(reasons.join(QStringLiteral(", "))).arg(overwrite).arg(preview);
}

QString normalizedScriptTarget(TransportKind kind, const QString& workingDirectory,
                               const QString& targetPath)
{
    if (workingDirectory.isEmpty() || targetPath.isEmpty())
        return {};
    if (workingDirectory.contains(QChar::Null) || targetPath.contains(QChar::Null))
        return {};
    if (kind == TransportKind::LocalShell) {
        const QFileInfo targetInfo(targetPath);
        return QDir::cleanPath(targetInfo.isAbsolute()
            ? targetInfo.absoluteFilePath()
            : QDir(workingDirectory).absoluteFilePath(targetPath));
    }
    if (kind == TransportKind::Ssh) {
        const QString joined = targetPath.startsWith(QLatin1Char('/'))
            ? targetPath : workingDirectory + QLatin1Char('/') + targetPath;
        return QDir::cleanPath(joined);
    }
    return {};
}

QString shellQuotePosix(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

std::optional<QString> scriptInvocationCommand(const QString& shellKind,
    const QString& workingDirectory, const QString& invocation)
{
    if (workingDirectory.isEmpty() || invocation.isEmpty()
        || workingDirectory.contains(QChar::Null)
        || workingDirectory.contains(QLatin1Char('\r'))
        || workingDirectory.contains(QLatin1Char('\n')))
        return std::nullopt;
    if (shellKind == QStringLiteral("posix")) {
        return QStringLiteral("cd -- %1 && %2")
            .arg(shellQuotePosix(workingDirectory), invocation);
    }
    if (shellKind == QStringLiteral("powershell")) {
        QString quoted = workingDirectory;
        quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QStringLiteral("Set-Location -LiteralPath '%1' -ErrorAction Stop; %2")
            .arg(quoted, invocation);
    }
    if (shellKind == QStringLiteral("cmd")) {
        if (workingDirectory.contains(QLatin1Char('"'))
            || workingDirectory.contains(QLatin1Char('%'))
            || workingDirectory.contains(QLatin1Char('^')))
            return std::nullopt;
        QString quoted = workingDirectory;
        quoted.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return QStringLiteral("cd /d \"%1\" && %2").arg(quoted, invocation);
    }
    return std::nullopt;
}


QJsonObject project(const CommandResult& result)
{
    QJsonObject data;
    if (result.execution) {
        const auto& value = *result.execution;
        data = {{"executionId", value.executionId}, {"instanceId", value.instanceId},
            {"sessionId", value.session.session.str()}, {"epoch", value.session.epoch.str()},
            {"commandId", value.commandId}, {"policyVersion", value.policyVersion},
            {"status", value.status}, {"executionMayHaveStarted", value.executionMayHaveStarted},
            {"terminationConfirmed", value.terminationConfirmed}, {"startedAt", QJsonValue(QJsonValue::Null)},
            {"finishedAt", value.finishedAt.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value.finishedAt)},
            {"exitCode", value.exitCode ? QJsonValue(*value.exitCode) : QJsonValue(QJsonValue::Null)},
            {"stdout", value.standardOutput}, {"stderr", value.standardError}, {"outputTruncated", value.outputTruncated}};
    }
    if (result.publication) {
        const auto& value = *result.publication;
        data.insert("executionId", value.executionId);
        data.insert("targetPath", value.targetPath);
        data.insert("status", value.status);
        data.insert("published", value.published);
        data.insert("publicationMayHaveOccurred", value.publicationMayHaveOccurred);
        data.insert("invocationStarted", value.invocationStarted);
    }
    if (!result.error) return success(data);
    auto payload = ToolCodec::encodeFailure(*result.error);
    if (!data.isEmpty()) {
        auto error = payload.value("error").toObject();
        error.insert("details", data); payload.insert("error", error);
    }
    return payload;
}
} // namespace

class CommandSessionAdapter::Impl final : public CommandSessionPort, public CommandTargetStore
{
public:
    Impl(AccessStore& access, SessionDirectory& sessions, QString directory, QString instance, Delivery delivery)
        : access(access), sessions(sessions), guard(std::move(directory)), delivery(std::move(delivery))
    {
        clock.start();
        runtime = std::make_unique<CommandRuntime>(*this, *this, std::move(instance), CommandRuntime::Delivery{
            [this](const TaskId& task, std::optional<CommandResult> result) {
                calls.remove(task.str());
                this->delivery.complete(task, result ? project(*result) : QJsonObject{});
            },
            this->delivery.deadline,
            [this](const TaskId& task, const CommandPrompt& prompt) { return confirmPrompt(task, prompt); },
            this->delivery.changed}, [this] { return clock.elapsed(); });
    }
    ~Impl() { runtime->stop(); }
    struct CallContext { QString peer, request; QByteArray key; };
    quint64 observe(QObject* object)
    {
        if (!object) return 0;
        if (auto id = objectIds.value(object)) return id;
        const auto id = nextChannel++;
        objectIds.insert(object, id); channels.insert(id, object);
        QObject::connect(object, &QObject::destroyed, &context, [this, object, id] {
            objectIds.remove(object); channels.remove(id);
            facades.remove(id); coordinators.remove(id);
            if (providers.remove(id)) runtime->providerUnavailable(id);
        });
        return id;
    }
    std::optional<CommandSessionFacts> session(const ClientId& client, const SessionId& id) override
    {
        const auto entry = sessions.find(id.str());
        if (!entry || !entry->session) return std::nullopt;
        CommandSessionFacts facts;
        facts.reference = {SessionId{entry->id}, Epoch{entry->epoch}};
        facts.kind = entry->kind == TransportKind::LocalShell ? CommandSessionKind::Local
            : entry->kind == TransportKind::Ssh ? CommandSessionKind::Ssh
            : entry->kind == TransportKind::Serial ? CommandSessionKind::Serial : CommandSessionKind::Other;
        facts.target = entry->targetFingerprint; facts.sessionGeneration = entry->generation;
        facts.grantVersion = access.version(client.str());
        facts.running = entry->state == SessionState::Running;
        facts.readable = access.canRead(client.str(), *entry);
        facts.commandAllowed = access.canRunCommand(client.str(), *entry);
        facts.scriptAllowed = access.canRunScriptTask(client.str(), *entry);
        auto* facade = entry->session->commandFacade();
        auto* coordinator = entry->session->commandCoordinator();
        auto* provider = entry->session->scriptProvider();
        const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
        facts.profileVersion = profile.version(); facts.diagnosticPolicyVersion = CommandPolicy::version(profile);
        facts.profileAvailable = profile.isAvailable();
        facts.executorAvailable = facade && facade->isAvailable();
        facts.coordinatorAvailable = coordinator != nullptr;
        facts.trustedProfile = coordinator && coordinator->hasTrustedProfile();
        facts.promptReady = coordinator && coordinator->isPromptReady();
        facts.promptFree = coordinator && ((entry->kind == TransportKind::Ssh && coordinator->allowsUnverifiedPrompt())
            || (entry->kind == TransportKind::Serial && coordinator->allowsIdleCompletion()));
        facts.promptGeneration = coordinator ? coordinator->promptGeneration() : 0;
        facts.userInputGeneration = entry->session->inputArbiter()->userInputGeneration();
        facts.providerAvailable = provider && provider->isAvailable();
        facts.mode = facade ? facade->capabilities().mode : CommandExecutionMode::Isolated;
        for (const auto& command : CommandPolicy::catalog(profile)) facts.diagnostics.insert(command.id, command.command);
        facts.allowedDiagnostics = access.commands(client.str(), *entry);
        facts.executor = observe(facade); facts.coordinator = observe(coordinator); facts.provider = observe(provider);
        if (facade && !facades.contains(facts.executor)) {
            facades.insert(facts.executor);
            QObject::connect(facade, &SessionCommandFacade::finished, &context,
                [this](const CommandExecutionResult& result) { runtime->executionFinished(result); });
        }
        if (coordinator && !coordinators.contains(facts.coordinator)) {
            coordinators.insert(facts.coordinator);
            QObject::connect(coordinator, &SessionCommandCoordinator::finished, &context,
                [this](const CommandExecutionResult& result) { runtime->executionFinished(result); });
        }
        if (provider && !providers.contains(facts.provider)) {
            providers.insert(facts.provider);
            QObject::connect(provider, &ISessionScriptProvider::finished, &context, [this](const ScriptWriteResult& result) {
                ScriptPublicationResult value;
                value.requestId = result.requestId; value.success = result.success;
                if (!result.errorCode.isEmpty()) value.error = errorCodeFromName(result.errorCode);
                value.resolvedTargetPath = result.resolvedTargetPath; value.contentHash = result.contentHash;
                runtime->publicationFinished(value);
            });
        }
        auto* arbiter = entry->session->inputArbiter();
        if (!arbiters.contains(arbiter)) {
            arbiters.insert(arbiter);
            QObject::connect(arbiter, &SessionInputArbiter::userInputStarted, &context,
                [this, id](quint64 generation) { runtime->userInput(id, generation); });
            QObject::connect(arbiter, &QObject::destroyed, &context, [this, arbiter] { arbiters.remove(arbiter); });
        }
        return facts;
    }
    Result<PreparedScript> prepareScript(const CommandSessionFacts& facts, const RunScriptRequest& request) override
    {
        const auto entry = sessions.find(facts.reference.session.str());
        if (!entry || !entry->session) return Error{ErrorCode::SessionNotAvailable};
        const auto target = normalizedScriptTarget(entry->kind, request.workingDirectory, request.targetPath);
        if (target.isEmpty()) return Error{ErrorCode::ScriptTargetInvalid};
        const auto command = scriptInvocationCommand(entry->session->runtimeConfig().transport.value("interactiveShellKind").toString(),
            request.workingDirectory, request.invocation);
        if (!command) return Error{ErrorCode::CommandScriptInvocationInvalid};
        return PreparedScript{target, *command, entry->kind == TransportKind::LocalShell && QFileInfo(target).exists()};
    }
    bool submit(quint64 channel, bool direct, const CommandExecutionRequest& request) override
    {
        auto* object = channels.value(channel).data();
        if (!object) return false;
        return direct ? static_cast<SessionCommandCoordinator*>(object)->submit(request)
            : static_cast<SessionCommandFacade*>(object)->execute(request);
    }
    void cancel(quint64 channel, bool direct, quint64 request, bool expired) override
    {
        auto* object = channels.value(channel).data();
        if (!object) return;
        if (direct) {
            auto* coordinator = static_cast<SessionCommandCoordinator*>(object);
            if (expired) coordinator->expire(request); else coordinator->cancel(request);
        } else static_cast<SessionCommandFacade*>(object)->cancel(request);
    }
    bool publish(quint64 provider, const ScriptPublicationRequest& request) override
    {
        auto* object = channels.value(provider).data();
        return object && static_cast<ISessionScriptProvider*>(object)->writeScript(
            {request.requestId, request.content, request.targetPath, request.workingDirectory});
    }
    void cancelPublication(quint64 provider, quint64 request) override
    {
        if (auto* object = channels.value(provider).data()) static_cast<ISessionScriptProvider*>(object)->cancelWrite(request);
    }
    Result<std::monostate> reserve(const TargetProtection& protection) override
    {
        QString reason;
        if (guard.reserve(protection.target, protection.execution, protection.instance, reason)) return std::monostate{};
        const auto code = errorCodeFromName(reason).value_or(ErrorCode::Busy);
        Error error{code}; error.retry = RetryHint{reason == "BUSY", std::chrono::milliseconds{250}};
        return error;
    }
    bool release(const QString& target, const QString& execution) override { return guard.release(target, execution); }
    QList<TargetProtection> protections() const override
    {
        QList<TargetProtection> values;
        const auto markers = guard.markers();
        for (auto it = markers.begin(); it != markers.end(); ++it) {
            const auto value = it.value().toObject();
            values.append({it.key(), value.value("executionId").toString(), value.value("instanceId").toString(), value.value("createdAt").toString()});
        }
        return values;
    }
    bool acknowledge(const QString& target) override { return guard.acknowledge(target); }
    bool confirmPrompt(const TaskId& task, const CommandPrompt& prompt)
    {
        const auto call = calls.value(task.str());
        QString message;
        if (const auto* script = std::get_if<RunScriptRequest>(&prompt.request))
            message = scriptConfirmationMessage(script->scriptContent.toUtf8(), prompt.resolvedTarget,
                script->workingDirectory, script->invocation, prompt.reasons, prompt.targetExistsLocally);
        else message = QStringLiteral("NovaTerm requests confirmation to run this terminal command:\n\n%1\n\nRisk: %2")
            .arg(std::get<RunCommandRequest>(prompt.request).command, prompt.reasons.join(", "));
        QJsonObject packet{{"op", prompt.detached ? "input_required" : "elicitation"}, {"id", call.request}, {"message", message}};
        if (prompt.detached) packet.insert("requestState", ToolCodec::signConfirmation(prompt.transaction, call.key));
        return delivery.send(call.peer, packet);
    }
    AccessStore& access;
    SessionDirectory& sessions;
    TargetGuard guard;
    Delivery delivery;
    QElapsedTimer clock;
    QHash<QString, CallContext> calls;
    QHash<QObject*, quint64> objectIds;
    QHash<quint64, QPointer<QObject>> channels;
    QSet<quint64> facades, coordinators, providers;
    QSet<SessionInputArbiter*> arbiters;
    quint64 nextChannel{1};
    std::unique_ptr<CommandRuntime> runtime;
    QObject context; // 先断开Session回调，再销毁运行期状态。
};

CommandSessionAdapter::CommandSessionAdapter(AccessStore& access, SessionDirectory& sessions,
    QString directory, QString instance, Delivery delivery)
    : _impl(std::make_unique<Impl>(access, sessions, std::move(directory), std::move(instance), std::move(delivery))) {}
CommandSessionAdapter::~CommandSessionAdapter() = default;
void CommandSessionAdapter::start(CommandCall call, const SessionDirectory::Entry&)
{
    CommandInvocation invocation;
    invocation.task = call.task; invocation.connection = PeerId{call.connection}; invocation.client = call.client;
    invocation.request = std::move(call.request); invocation.confirmationDigest = std::move(call.confirmationDigest);
    invocation.answer = call.answer; invocation.detachedConfirmation = call.mrtr; invocation.formElicitation = call.formElicitation;
    invocation.confirmationSupplied = !call.requestState.isEmpty() || call.answer.has_value();
    if (!call.requestState.isEmpty()) invocation.confirmation = ToolCodec::verifyConfirmation(call.requestState, call.connectionKey);
    if (const auto* request = std::get_if<ExecuteCommandRequest>(&invocation.request)) {
        if (const auto token = verifyToken(request->commandTicket, call.connectionKey)) {
            if (token->value("kind") == "command") invocation.ticket = DiagnosticTicket{
                token->value("execution").toString(), {SessionId{token->value("session").toString()}, Epoch{token->value("epoch").toString()}},
                DiagnosticId{token->value("command").toString()}, token->value("policy").toString(), token->value("profile").toString(),
                ClientAccessVersion{token->value("grant").toString().toULongLong()}, qint64(token->value("expires").toDouble())};
        }
    }
    _impl->calls.insert(call.task.str(), {call.connection, call.id, call.connectionKey});
    _impl->runtime->start(std::move(invocation));
}
void CommandSessionAdapter::confirm(const QString& peer, const QString& request, ConfirmationAnswer answer) { _impl->runtime->confirm(TaskId{peer + '/' + request}, answer); }
void CommandSessionAdapter::cancel(const TaskId& task) { _impl->calls.remove(task.str()); _impl->runtime->cancel(task); }
void CommandSessionAdapter::expire(const TaskId& task) { _impl->runtime->expire(task); }
void CommandSessionAdapter::disconnect(const QString& peer) { _impl->runtime->disconnect(peer); }
void CommandSessionAdapter::revoke(const QString& client, const QString& session) { _impl->runtime->revoke(client, session); }
void CommandSessionAdapter::stop() { _impl->runtime->stop(); }
void CommandSessionAdapter::sweep() { _impl->runtime->sweep(); }
bool CommandSessionAdapter::idle() const { return _impl->runtime->idle(); }
bool CommandSessionAdapter::quarantined(const QString& target) const { return _impl->runtime->quarantined(target); }
QJsonObject CommandSessionAdapter::protectedTargets() const { return _impl->guard.markers(); }
bool CommandSessionAdapter::acknowledgeTarget(const QString& target, const QString& expected) { return _impl->runtime->acknowledgeTarget(target, expected); }
QJsonArray CommandSessionAdapter::executionRecords() const
{
    QJsonArray values;
    for (const auto& record : _impl->runtime->executionRecords()) {
        const char* phase = record.phase == TaskPhase::Final ? "final" : record.phase == TaskPhase::Observing ? "observing" : "submitted";
        values.append(QJsonObject{{"executionId", record.executionId}, {"commandId", record.commandId}, {"sessionId", record.session.session.str()},
            {"complete", record.complete}, {"phase", phase}, {"result", project(record.result)}});
    }
    return values;
}
} // namespace NovaTerm::Mcp
