/** @file ScriptCommandUseCase.cpp @brief 脚本/自由命令用例实现。 */
#include "ScriptCommandUseCase.h"

#include "ScriptInvocation.h"

#include "ConfirmationRegistry.h"
#include "mcp/McpAccess.h"
#include "mcp/McpProtocol.h"
#include "mcp/contracts/Budget.h"
#include "mcp/CommandPolicy.h"
#include "mcp/CommandRiskPolicy.h"
#include "session/ISessionScriptProvider.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionInputArbiter.h"
#include "session/TerminalSession.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace NovaTerm::Mcp {
namespace {
QString versionString(quint64 version) { return QString::number(version); }
QString jobKey(const QString& connection, const QString& id)
{
    return connection + QLatin1Char('/') + id;
}
QJsonObject error(const char* code, bool retryable = false, int retryAfter = 0)
{
    return failure(QString::fromLatin1(code), QString::fromLatin1(code),
        retryable, retryAfter);
}

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
} // namespace

ScriptCommandUseCase::ScriptCommandUseCase(Ports ports) : _ports(std::move(ports)) {}

void ScriptCommandUseCase::runScript(const std::shared_ptr<Job>& job,
    const SessionDirectory::Entry& entry)
{
    const auto connection = ConnectionView{_ports.clientFor ? _ports.clientFor(job->connection) : QString{}, _ports.keyFor ? _ports.keyFor(job->connection) : QByteArray{}};
    if (!_ports.access->canRunScriptTask(connection.client, entry)) {
        _ports.complete(job, !job->requestState.isEmpty()
            ? error("COMMAND_CONFIRMATION_STALE")
            : error("SCRIPT_PERMISSION_REQUIRED"));
        return;
    }
    if (entry.state != SessionState::Running || !entry.session) {
        _ports.complete(job, error("SESSION_NOT_READY"));
        return;
    }
    auto* provider = entry.session->scriptProvider();
    if (!provider || !provider->isAvailable()) {
        _ports.complete(job, error("SCRIPT_PROVIDER_UNAVAILABLE"));
        return;
    }
    auto* coordinator = entry.session->commandCoordinator();
    auto* facade = entry.session->commandFacade();
    const bool localIsolated = entry.kind == TransportKind::LocalShell
        && facade && facade->capabilities().mode == CommandExecutionMode::Isolated;
    if (!coordinator || !coordinator->hasTrustedProfile() || !facade
        || !facade->isAvailable()
        || (facade->capabilities().mode != CommandExecutionMode::InteractiveFramed
            && !localIsolated)) {
        _ports.complete(job, error("COMMAND_PROFILE_UNAVAILABLE"));
        return;
    }

    const auto& typed = std::get<RunScriptRequest>(job->request);
    const QByteArray content = typed.scriptContent.toUtf8();
    const QString targetPath = typed.targetPath;
    const QString workingDirectory = typed.workingDirectory;
    const QString invocation = typed.invocation;
    const auto risk = CommandRiskPolicy{}.classifyScript(
        content, targetPath, workingDirectory, invocation);
    if (risk.decision == RiskDecision::Deny) {
        _ports.complete(job, error("COMMAND_NOT_ALLOWED"));
        return;
    }
    if (risk.decision != RiskDecision::Confirm) {
        _ports.complete(job, error("COMMAND_SCRIPT_RISK_UNKNOWN"));
        return;
    }
    const QString displayTarget = normalizedScriptTarget(
        entry.kind, workingDirectory, targetPath);
    if (displayTarget.isEmpty()) {
        _ports.complete(job, error("SCRIPT_TARGET_INVALID"));
        return;
    }
    const bool targetExistsLocally = entry.kind == TransportKind::LocalShell
        && QFileInfo(displayTarget).exists();
    if (!coordinator->isPromptReady()) {
        _ports.complete(job, error("SESSION_COMMAND_NOT_READY"));
        return;
    }
    const auto profile = facade->profile();
    const QByteArray argumentHash = job->confirmationDigest;

    if (!job->confirmationAccepted) {
        if (!job->formElicitation && job->requestState.isEmpty()) {
            _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
            return;
        }
        if (!job->requestState.isEmpty() || !job->inputResponses.isEmpty()) {
            const auto state = verifyToken(job->requestState, connection.key);
            const auto response = job->inputResponses.value("confirm").toObject();
            const QString nonce = state ? state->value("nonce").toString() : QString{};
            const bool valid = job->mrtr && state
                && state->value("kind") == QStringLiteral("interactive-confirmation")
                && state->value("tool") == QStringLiteral("novaterm_run_script")
                && state->value("client") == connection.client
                && state->value("connection") == job->connection
                && state->value("session") == entry.id
                && state->value("epoch") == entry.epoch
                && state->value("target") == entry.targetFingerprint
                && state->value("payloadHash").toString()
                    == QString::fromLatin1(argumentHash.toHex())
                && state->value("policyVersion") == risk.policyVersion
                && state->value("profileVersion") == profile.version()
                && state->value("grantVersion")
                    == versionString(_ports.access->version(connection.client))
                && _ports.access->canRunScriptTask(connection.client, entry)
                && state->value("promptGeneration").toString()
                    == QString::number(coordinator->promptGeneration())
                && state->value("expires").toDouble() > _ports.now()
                && !nonce.isEmpty()
                && _ports.confirmations->isActive(job->connection, nonce)
                && !_ports.confirmations->isConsumed(nonce);
            if (!valid) {
                _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
                return;
            }
            _ports.confirmations->consumeActive(job->connection, nonce,
                qint64(state->value("expires").toDouble()));
            const QString action = response.value("action").toString();
            if (action != QStringLiteral("accept")
                || !response.value("content").toObject()
                    .value("confirmed").toBool()) {
                _ports.complete(job, error(action == QStringLiteral("cancel")
                    ? "COMMAND_CONFIRMATION_CANCELLED"
                    : "COMMAND_CONFIRMATION_DECLINED"));
                return;
            }
            job->confirmationAccepted = true;
            job->requestState.clear();
            job->inputResponses = {};
        } else if (job->mrtr) {
            if (_ports.confirmations->hasActiveForConnection(job->connection)) {
                _ports.complete(job, error("BUSY", true, 250));
                return;
            }
            const QString nonce = newId();
            const QJsonObject state{{"kind", "interactive-confirmation"},
                {"tool", "novaterm_run_script"},
                {"client", connection.client}, {"connection", job->connection},
                {"session", entry.id}, {"epoch", entry.epoch},
                {"target", entry.targetFingerprint},
                {"payloadHash", QString::fromLatin1(argumentHash.toHex())},
                {"policyVersion", risk.policyVersion},
                {"profileVersion", profile.version()},
                {"grantVersion", versionString(_ports.access->version(connection.client))},
                {"promptGeneration", QString::number(coordinator->promptGeneration())},
                {"nonce", nonce}, {"expires", double(_ports.now() + Budget::ConfirmationTimeout.count())}};
            const QString token = signToken(state, connection.key);
            const QString message = scriptConfirmationMessage(
                content, displayTarget, workingDirectory, invocation,
                risk.reasons, targetExistsLocally);
            const qint64 expires = _ports.now() + Budget::ConfirmationTimeout.count();
            _ports.confirmations->registerActive(job->connection, connection.client,
                entry.id, nonce, expires);
            if (!_ports.send(job->connection, {{"op", "input_required"},
                    {"id", job->id}, {"message", message}, {"requestState", token}})) {
                _ports.confirmations->clearActive(job->connection, nonce);
                _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                return;
            }
            job->cancelled = true;
            _ports.complete(job, {});
            return;
        } else {
            if (_ports.confirmations->hasPendingForConnection(job->connection)) {
                _ports.complete(job, error("BUSY", true, 250));
                return;
            }
            const QString pendingKey = jobKey(job->connection, job->id);
            PendingConfirmationRecord pending;
            pending.jobKey = pendingKey;
            pending.connection = job->connection;
            pending.client = connection.client;
            pending.session = entry.id;
            pending.epoch = entry.epoch;
            pending.command = invocation;
            pending.policyVersion = risk.policyVersion;
            pending.tool = job->tool;
            pending.profileVersion = profile.version();
            pending.targetFingerprint = entry.targetFingerprint;
            pending.grantVersion = _ports.access->version(connection.client);
            pending.promptGeneration = coordinator->promptGeneration();
            pending.payloadHash = argumentHash;
            pending.expires = _ports.now() + Budget::ConfirmationTimeout.count();
            _ports.confirmations->insertPending(pendingKey, pending);
            const QString message = scriptConfirmationMessage(
                content, displayTarget, workingDirectory, invocation,
                risk.reasons, targetExistsLocally);
            if (!_ports.send(job->connection, {{"op", "elicitation"},
                    {"id", job->id}, {"message", message}})) {
                _ports.confirmations->removePending(pendingKey);
                _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                return;
            }
            job->deadline = _ports.now() + Budget::ConfirmationTimeout.count();
            return;
        }
    }

    const bool readyAfterConfirmation = coordinator->isPromptReady();
    const bool stillAuthorized = _ports.access->canRunScriptTask(connection.client, entry);
    const bool sameProvider = provider == entry.session->scriptProvider();
    const bool providerAvailable = provider->isAvailable();
    if (!readyAfterConfirmation || !stillAuthorized || !sameProvider
        || !providerAvailable) {
        _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
        return;
    }
    if (_ports.guards->markers().contains(entry.targetFingerprint)) {
        _ports.complete(job, error("COMMAND_EXECUTION_QUARANTINED"));
        return;
    }
    if (_ports.dedup->blocked(entry.targetFingerprint)
        || _ports.scriptTasks->size() >= MaxClients * MaxActiveRequests
        || _ports.executions->size() >= Budget::MaxExecutionRecords) {
        _ports.complete(job, error("BUSY", true, 5000));
        return;
    }
    auto* inputArbiter = entry.session->inputArbiter();
    if (!inputArbiter) {
        _ports.complete(job, error("SESSION_COMMAND_NOT_READY"));
        return;
    }
    if (!_ports.observedArbiters->contains(inputArbiter)) {
        _ports.observedArbiters->insert(inputArbiter);
        QObject::connect(inputArbiter, &SessionInputArbiter::userInputStarted, &_connectionGuard,
            [cancel = _ports.cancelByUser, session = entry.session.data()](quint64 userGeneration) {
                if (cancel)
                    cancel(session, userGeneration);
            });
        QObject::connect(inputArbiter, &QObject::destroyed, &_connectionGuard,
            [this, inputArbiter] { _ports.observedArbiters->remove(inputArbiter); });
    }
    const QString pathLock = entry.targetFingerprint + QLatin1Char('/')
        + displayTarget;
    if (_ports.scriptTasks->pathLocked(pathLock)) {
        _ports.complete(job, error("BUSY", true, 500));
        return;
    }
    const auto command = scriptInvocationCommand(
        entry.session->runtimeConfig().transport.value(
            QStringLiteral("interactiveShellKind")).toString(),
        workingDirectory, invocation);
    if (!command || command->toUtf8().size() > 16384) {
        _ports.complete(job, error("COMMAND_SCRIPT_INVOCATION_INVALID"));
        return;
    }
    const QString owner = jobKey(job->connection, job->id);
    const quint64 requestId = (*_ports.nextScriptWrite)++;
    ScriptTaskRegistry::Task task;
    task.job = job;
    task.provider = provider;
    task.client = connection.client;
    task.session = entry.id;
    task.epoch = entry.epoch;
    task.targetFingerprint = entry.targetFingerprint;
    task.policyVersion = risk.policyVersion;
    task.profileVersion = profile.version();
    task.targetPath = normalizedScriptTarget(entry.kind, workingDirectory, targetPath);
    task.workingDirectory = workingDirectory;
    task.invocation = invocation;
    task.scriptPathLock = pathLock;
    task.metadataHash = job->scriptMetadataHash;
    task.contentHash = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
    task.promptGeneration = coordinator->promptGeneration();
    task.grantVersion = _ports.access->version(connection.client);
    task.sessionGeneration = entry.generation;
    task.userInputGeneration = inputArbiter->userInputGeneration();
    if (!_ports.observedProviders->contains(provider)) {
        _ports.observedProviders->insert(provider);
        QObject::connect(provider, &ISessionScriptProvider::finished, &_connectionGuard,
            [finish = _ports.finishScriptWrite](const ScriptWriteResult& result) {
                if (finish) finish(result);
            });
        QObject::connect(provider, &QObject::destroyed, &_connectionGuard,
            [this, provider] {
                _ports.observedProviders->remove(provider);
                const auto ids = _ports.scriptTasks->ids();
                for (const auto id : ids) {
                    const auto* current = _ports.scriptTasks->find(id);
                    if (!current || (current->provider.data() != provider
                        && !current->provider.isNull()))
                        continue;
                    auto taken = _ports.scriptTasks->take(id);
                    if (!taken)
                        continue;
                    const auto task = *taken;
                    const auto job = task.job;
                    ScriptWriteResult uncertain;
                    uncertain.requestId = id;
                    uncertain.errorCode = QStringLiteral("SCRIPT_PUBLICATION_OUTCOME_UNKNOWN");
                    _ports.recordOutcome(task, uncertain);
                    // Provider 消失后不能证明远端未提交，保留持久保护标记。
                    _ports.releaseReservation(job, false);
                    _ports.complete(job, error("SCRIPT_PROVIDER_UNAVAILABLE"));
                }
            });
    }
    QString rejected;
    const QString reservationId = newId();
    auto reservation = TargetReservation::acquire(*_ports.guards, entry.targetFingerprint,
        reservationId, _ports.instance, rejected);
    if (!reservation) {
        _ports.complete(job, failure(rejected, rejected, rejected == QStringLiteral("BUSY"), 250));
        return;
    }
    job->reservation = std::move(reservation);
    static_cast<void>(_ports.scriptTasks->lockPath(pathLock, owner));
    _ports.scriptTasks->insert(requestId, task);
    ScriptWriteRequest request{requestId, content, targetPath, workingDirectory};
    job->deadline = _ports.now() + ScriptWriteTimeout.count();
    if (!provider->writeScript(request)) {
        _ports.scriptTasks->remove(requestId);
        _ports.complete(job, error("SCRIPT_WRITE_FAILED"));
        return;
    }
    job->scriptPolicyVersion = risk.policyVersion;
}


void ScriptCommandUseCase::runCommand(const std::shared_ptr<Job>& job,
const SessionDirectory::Entry& entry)
{
    const auto connection = ConnectionView{_ports.clientFor ? _ports.clientFor(job->connection) : QString{}, _ports.keyFor ? _ports.keyFor(job->connection) : QByteArray{}};
    // 「允许交互命令」只约束 novaterm_run_command。脚本任务有自己的独立授权位
    // （scriptTask）与逐次人类确认；其调用命令虽然同样会打进当前终端，但那已经
    // 由「脚本授权 + 每次确认」显式承担。若在这里一并要求交互命令授权，
    // 「只授脚本任务」的合法配置会在脚本正文已落盘之后才失败，且报出
    // COMMAND_PERMISSION_REQUIRED 而不是 SCRIPT_PERMISSION_REQUIRED。
    // novaterm_run_script 走 runScript 入口的 canRunScriptTask 检查。
    if (job->tool == QStringLiteral("novaterm_run_command")
        && !_ports.access->canRunCommand(connection.client, entry)) {
        _ports.complete(job, !job->requestState.isEmpty()
            ? error("COMMAND_CONFIRMATION_STALE")
            : error("COMMAND_PERMISSION_REQUIRED"));
        return;
    }
    if (entry.state != SessionState::Running || !entry.session) {
        _ports.complete(job, error("SESSION_NOT_READY"));
        return;
    }
    const QString command = std::get_if<RunCommandRequest>(&job->request)
        ? std::get<RunCommandRequest>(job->request).command : job->resolvedCommand;
    const RiskAssessment risk = CommandRiskPolicy{}.classify(command);
    if (risk.decision == RiskDecision::Deny) {
        _ports.complete(job, error("COMMAND_NOT_ALLOWED"));
        return;
    }
    if (risk.decision == RiskDecision::Allow
        && (!job->requestState.isEmpty() || !job->inputResponses.isEmpty())) {
        _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
        return;
    }
    if (risk.decision != RiskDecision::Allow && !job->confirmationAccepted
        && !job->formElicitation && job->requestState.isEmpty()) {
        _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
        return;
    }
    auto* coordinator = entry.session->commandCoordinator();
    const bool unverifiedSsh = entry.kind == TransportKind::Ssh
        && coordinator && coordinator->allowsUnverifiedPrompt();
    const bool idleSerial = entry.kind == TransportKind::Serial
        && coordinator && coordinator->allowsIdleCompletion();
    const bool promptFree = unverifiedSsh || idleSerial;
    if (!coordinator || (!coordinator->hasTrustedProfile() && !promptFree)) {
        _ports.complete(job, error("COMMAND_PROFILE_UNAVAILABLE"));
        return;
    }
    if (!promptFree && !coordinator->isPromptReady()) {
        _ports.complete(job, error("SESSION_COMMAND_NOT_READY"));
        return;
    }
    auto* facade = entry.session->commandFacade();
    // LocalShell 的固定诊断使用隔离 helper，交互命令与脚本仍由协调器
    // 写入当前终端，避免把自由命令交给只接受四个 commandId 的 helper。
    const bool directCoordinator = promptFree
        || (entry.kind == TransportKind::LocalShell && facade
            && facade->capabilities().mode == CommandExecutionMode::Isolated);
    const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
    if (risk.decision != RiskDecision::Allow && !job->confirmationAccepted) {
        if (!job->formElicitation && job->requestState.isEmpty()) {
            _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
            return;
        }
        if (!job->requestState.isEmpty() || !job->inputResponses.isEmpty()) {
            const auto state = verifyToken(job->requestState, connection.key);
            const auto response = job->inputResponses.value("confirm").toObject();
            const QString action = response.value("action").toString();
            const QString expectedDecision = risk.decision == RiskDecision::Confirm
                ? QStringLiteral("confirm") : QStringLiteral("unknown");
            const QString commandHash = QString::fromLatin1(
                QCryptographicHash::hash(command.toUtf8(), QCryptographicHash::Sha256).toHex());
            const QString nonce = state ? state->value("nonce").toString() : QString{};
            const bool valid = job->mrtr && state
                && state->value("kind") == QStringLiteral("interactive-confirmation")
                && state->value("client") == connection.client
                && state->value("connection") == job->connection
                && state->value("session") == entry.id
                && state->value("epoch") == entry.epoch
                && state->value("target") == entry.targetFingerprint
                && state->value("commandHash") == commandHash
                && state->value("policyVersion") == risk.policyVersion
                && state->value("decision") == expectedDecision
                && state->value("profileVersion") == profile.version()
                && state->value("grantVersion") == versionString(_ports.access->version(connection.client))
                && state->value("promptGeneration").toString()
                    == QString::number(coordinator->promptGeneration())
                && state->value("userInputGeneration").toString()
                    == QString::number(entry.session->inputArbiter()->userInputGeneration())
                && state->value("expires").toDouble() > _ports.now()
                && _ports.confirmations->isActive(job->connection, nonce)
                && !nonce.isEmpty() && !_ports.confirmations->isConsumed(nonce);
            if (!valid) {
                _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
                return;
            }
            _ports.confirmations->consumeActive(job->connection, nonce,
                qint64(state->value("expires").toDouble()));
            if ((!promptFree && !coordinator->isPromptReady())
                || coordinator->promptGeneration() != state->value("promptGeneration").toString().toULongLong()) {
                _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
                return;
            }
            if (action != QStringLiteral("accept")
                || !response.value("content").toObject().value("confirmed").toBool()) {
                _ports.complete(job, error(action == QStringLiteral("cancel")
                    ? "COMMAND_CONFIRMATION_CANCELLED" : "COMMAND_CONFIRMATION_DECLINED"));
                return;
            }
            job->confirmationAccepted = true;
        } else if (job->mrtr) {
            if (_ports.confirmations->hasActiveForConnection(job->connection)) {
                _ports.complete(job, error("BUSY", true, 250));
                return;
            }
            const QString decision = risk.decision == RiskDecision::Confirm
                ? QStringLiteral("confirm") : QStringLiteral("unknown");
            const QString nonce = newId();
            const QJsonObject state{{"kind", "interactive-confirmation"},
                {"client", connection.client}, {"connection", job->connection},
                {"session", entry.id}, {"epoch", entry.epoch},
                {"target", entry.targetFingerprint},
                {"commandHash", QString::fromLatin1(QCryptographicHash::hash(
                    command.toUtf8(), QCryptographicHash::Sha256).toHex())},
                {"policyVersion", risk.policyVersion}, {"decision", decision},
                {"profileVersion", profile.version()},
                {"grantVersion", versionString(_ports.access->version(connection.client))},
                {"promptGeneration", QString::number(coordinator->promptGeneration())},
                {"userInputGeneration", QString::number(entry.session->inputArbiter()->userInputGeneration())},
                {"nonce", nonce}, {"expires", double(_ports.now() + Budget::ConfirmationTimeout.count())}};
            const QString token = signToken(state, connection.key);
            const qint64 expires = _ports.now() + Budget::ConfirmationTimeout.count();
            _ports.confirmations->registerActive(job->connection, connection.client,
                entry.id, nonce, expires);
            const QString message = QStringLiteral("NovaTerm requests confirmation to run this terminal command:\n\n%1\n\nRisk: %2")
                .arg(command, risk.reasons.join(QStringLiteral(", ")));
            if (!_ports.send(job->connection, {{"op", "input_required"},
                    {"id", job->id}, {"message", message}, {"requestState", token}})) {
                _ports.confirmations->clearActive(job->connection, nonce);
                _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                return;
            }
            job->cancelled = true;
            _ports.complete(job, {});
            return;
        }
        if (job->confirmationAccepted) {
            // MRTR 回合在这里重新进入与 2025 accept 相同的执行分支。
        } else {
        if (_ports.confirmations->hasPendingForConnection(job->connection)) {
            _ports.complete(job, error("BUSY", true, 250));
            return;
        }
        const QString pendingKey = jobKey(job->connection, job->id);
        PendingConfirmationRecord pending;
        pending.jobKey = pendingKey;
        pending.connection = job->connection;
        pending.client = connection.client;
        pending.session = entry.id;
        pending.epoch = entry.epoch;
        pending.command = command;
        pending.policyVersion = risk.policyVersion;
        pending.tool = job->tool;
        pending.profileVersion = profile.version();
        pending.targetFingerprint = entry.targetFingerprint;
        pending.grantVersion = _ports.access->version(connection.client);
        pending.promptGeneration = coordinator->promptGeneration();
        pending.userInputGeneration = entry.session->inputArbiter()->userInputGeneration();
        pending.payloadHash = QCryptographicHash::hash(command.toUtf8(),
            QCryptographicHash::Sha256);
        pending.expires = _ports.now() + Budget::ConfirmationTimeout.count();
        _ports.confirmations->insertPending(pendingKey, pending);
        const QString message = QStringLiteral("NovaTerm requests confirmation to run this terminal command:\n\n%1\n\nRisk: %2")
            .arg(command, risk.reasons.join(QStringLiteral(", ")));
        if (!_ports.send(job->connection, {{"op", "elicitation"},
                {"id", job->id}, {"message", message}})) {
            _ports.confirmations->removePending(pendingKey);
            _ports.complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
            return;
        }
        job->deadline = _ports.now() + Budget::ConfirmationTimeout.count();
        return;
        }
    }
    if (!facade || !facade->isAvailable()
        || entry.targetFingerprint.isEmpty()) {
        _ports.complete(job, error("COMMAND_UNAVAILABLE"));
        return;
    }
    const auto marker = _ports.guards->markers().value(entry.targetFingerprint).toObject();
    const bool ownsReservation = job->reservation
        && job->reservation->fingerprint() == entry.targetFingerprint
        && marker.value("executionId").toString() == job->reservation->executionId();
    if (job->reservation && !ownsReservation) {
        _ports.complete(job, error("COMMAND_CONFIRMATION_STALE"));
        return;
    }
    if (!marker.isEmpty() && !ownsReservation) {
        _ports.complete(job, error("COMMAND_EXECUTION_QUARANTINED"));
        return;
    }
    const int clientExecutions = _ports.executions->countForClient(connection.client);
    if (clientExecutions >= Budget::MaxExecutionRecordsPerClient
        || _ports.executions->size() + _ports.scriptTasks->size() >= Budget::MaxExecutionRecords
        || _ports.dedup->blocked(entry.targetFingerprint)) {
        _ports.complete(job, error("BUSY", true, 5000));
        return;
    }
    const QString executionId = ownsReservation
        ? job->reservation->executionId() : newId();
    QString rejected;
    std::unique_ptr<TargetReservation> reservation;
    if (!ownsReservation) {
        reservation = TargetReservation::acquire(*_ports.guards, entry.targetFingerprint,
            executionId, _ports.instance, rejected);
        if (!reservation) {
            _ports.complete(job, error("BUSY", true, 250));
            return;
        }
    }
    if (!_ports.observedFacades->contains(facade)) {
        _ports.observedFacades->insert(facade);
        QObject::connect(facade, &SessionCommandFacade::finished, &_connectionGuard,
            [this](const CommandExecutionResult& result) {
                _ports.finished(result);
            });
        QObject::connect(facade, &QObject::destroyed, &_connectionGuard,
            [this, facade] { _ports.observedFacades->remove(facade); });
    }
    const quint64 requestId = (*_ports.nextCommandRequest)++;
    std::chrono::milliseconds timeout{Budget::CommandTimeout};
    if (const auto* run = std::get_if<RunCommandRequest>(&job->request))
        timeout = run->timeout;
    else if (const auto* script = std::get_if<RunScriptRequest>(&job->request))
        timeout = script->timeout;
    const QString executionCommandId = job->tool == QStringLiteral("novaterm_run_script")
        ? QStringLiteral("interactive.script")
        : QStringLiteral("interactive.free");
    const QString executionPolicyVersion = job->tool == QStringLiteral("novaterm_run_script")
        && !job->scriptPolicyVersion.isEmpty()
        ? job->scriptPolicyVersion : risk.policyVersion;
    ExecutionRecord execution;
    execution.id = executionId;
    execution.connection = job->connection;
    execution.client = connection.client;
    execution.session = entry.id;
    execution.epoch = entry.epoch;
    execution.commandId = executionCommandId;
    execution.policyVersion = executionPolicyVersion;
    execution.target = entry.targetFingerprint;
    execution.executorRequestId = requestId;
    execution.submitted = _ports.now();
    execution.facade = facade;
    execution.jobKey = jobKey(job->connection, job->id);
    execution.interactive = true;
    execution.phase = TaskPhase::Submitted;
    _ports.executions->insert(executionId, execution);
    // 上传预留移交给执行记录（按 target/id 解除），owner 不释放标记。
    job->reservation.reset();
    if (directCoordinator) {
        if (auto* stored = _ports.executions->find(executionId))
            stored->coordinator = coordinator;
        if (!_ports.observedCoordinators->contains(coordinator)) {
            _ports.observedCoordinators->insert(coordinator);
            QObject::connect(coordinator, &SessionCommandCoordinator::finished, &_connectionGuard,
                [this](const CommandExecutionResult& result) {
                    _ports.finished(result);
                });
            QObject::connect(coordinator, &QObject::destroyed, &_connectionGuard,
                [this, coordinator] { _ports.observedCoordinators->remove(coordinator); });
        }
    }
    CommandExecutionRequest request;
    request.requestId = requestId;
    request.command = command.toUtf8();
    request.limits = {CommandOutputBytes, static_cast<int>(timeout.count())};
    request.commandId = executionCommandId;
    request.executionNonce = newId().toUtf8();
    request.expectedPromptGeneration = promptFree
        ? 0 : coordinator->promptGeneration();
    const bool submitted = directCoordinator
        ? coordinator->submit(request) : facade->execute(request);
    if (!submitted) {
        _ports.executions->remove(executionId);
        reservation.reset(); // 由下面的显式 release 统一走确定未开始路径
        _ports.guards->release(entry.targetFingerprint, executionId);
        _ports.complete(job, error("SESSION_COMMAND_BUSY"));
        return;
    }
    job->scriptPublished = false;
    static_cast<void>(_ports.executions->advancePhase(executionId,
        TaskPhase::Submitted, TaskPhase::Observing));
    _ports.dedup->remember(entry.targetFingerprint, _ports.now());
    job->deadline = _ports.now() + timeout.count();
    if (_ports.changed) _ports.changed();
}


} // namespace NovaTerm::Mcp
