/** @file McpService.cpp
 *  @brief 有界 MCP 请求处理；所有 Session 访问均位于 GUI 线程。
 */
#include "McpService.h"
#include "McpProtocol.h"
#include "CommandPolicy.h"
#include "CommandRiskPolicy.h"
#include "LocalMcpServer.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionInputArbiter.h"
#include "session/ISessionScriptProvider.h"
#include "core/ThreadNaming.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>

#ifndef NOVATERM_VERSION
#define NOVATERM_VERSION "0.2.30"
#endif

namespace NovaTerm::Mcp {
namespace {
constexpr int CancelCompletionGraceMs = 250;
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString jobKey(const QString& connection, const QString& id) { return connection + '/' + id; }
QString versionString(quint64 version) { return QString::number(version); }
QJsonObject error(const char* code, bool retryable = false, int retryAfter = 0)
{
    return failure(QString::fromLatin1(code), QString::fromLatin1(code), retryable, retryAfter);
}
QByteArray scriptArgumentsDigest(const QJsonObject& arguments)
{
    return QCryptographicHash::hash(
        QJsonDocument(arguments).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256);
}
QByteArray scriptMetadataDigest(QJsonObject arguments)
{
    arguments.remove(QStringLiteral("scriptContent"));
    return scriptArgumentsDigest(arguments);
}
QString safeScriptPreview(const QByteArray& content, qsizetype maximumBytes)
{
    const QString decoded = QString::fromUtf8(content.left(maximumBytes));
    QString preview;
    preview.reserve(decoded.size());
    for (const QChar character : decoded) {
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
        return QStringLiteral("cd /d \"%1\" && %2")
            .arg(quoted, invocation);
    }
    return std::nullopt;
}
QString scriptConfirmationMessage(const QByteArray& content,
    const QString& targetPath, const QString& workingDirectory,
    const QString& invocation, const QStringList& reasons,
    bool targetExistsLocally)
{
    constexpr qsizetype PreviewBytes = 4096;
    const qsizetype previewLength = qMin(content.size(), PreviewBytes);
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
}

class Service::Impl
{
public:
    struct Job {
        QString connection;
        QString id;
        QString tool;
        QJsonObject arguments;
        QString requestState;
        QJsonObject inputResponses;
        qint64 deadline{0};
        qint64 created{0};
        bool running{false};
        bool confirmationAccepted{false};
        bool mrtr{false};
        bool formElicitation{false};
        QString scriptPolicyVersion;
        std::atomic<bool> cancelled{false};
    };
    struct Connection {
        QString client;
        QByteArray key;
        int active{0};
        std::deque<std::shared_ptr<Job>> waiting;
    };
    struct PendingConfirmation {
        std::shared_ptr<Job> job;
        QString client, session, epoch, command, policyVersion, tool;
        QString profileVersion, targetFingerprint;
        quint64 grantVersion{0};
        quint64 promptGeneration{0};
        quint64 userInputGeneration{0};
        QByteArray payloadHash;
        qint64 expires{0};
    };
    struct ConfirmationScope {
        QString connection, client, session;
        qint64 expires{0};
    };
    struct Capture {
        QString connection;
        QString session;
        QString epoch;
        QString revision;
        QJsonArray viewport;
        QJsonArray recentOutput;
        std::shared_ptr<const TerminalContextProvider::Snapshot> base;
        bool sourceTruncated{false};
        bool outputTruncated{false};
        qint64 expires{0};
        qint64 created{0};
    };
    struct Execution {
        QString id;
        QString connection;
        QString client;
        QString session;
        QString epoch;
        QString commandId;
        QString policyVersion;
        QString target;
        quint64 executorRequestId{0};
        qint64 submitted{0};
        QPointer<SessionCommandFacade> facade;
        std::shared_ptr<Job> job;
        QJsonObject payload;
        bool complete{false};
        bool interactive{false};
        QString scriptPathLock;
        QPointer<SessionCommandCoordinator> coordinator;
    };
    struct ScriptWriteTask {
        std::shared_ptr<Job> job;
        QPointer<ISessionScriptProvider> provider;
        QString client, session, epoch, targetFingerprint;
        QString policyVersion, profileVersion, targetPath, workingDirectory;
        QString invocation, scriptPathLock;
        QByteArray metadataHash, contentHash;
        quint64 promptGeneration{0}, grantVersion{0}, sessionGeneration{0};
        quint64 userInputGeneration{0};
        int timeoutMs{5000};
    };

    Impl(Service* owner, QString stateDir, QString runtimeDir, std::unique_ptr<CredentialStore> credentials)
        : q(owner), stateDirectory(stateDir.isEmpty() ? dataDirectory() : std::move(stateDir)),
          instanceDirectory(runtimeDir.isEmpty() ? runtimeDirectory() : std::move(runtimeDir)),
          access(stateDirectory, std::move(credentials), owner),
          sessions(owner), io(owner), guards(QDir(stateDirectory).filePath("targets"))
    {
        clock.start();
        workers.setMaxThreadCount(1);
        workers.setExpiryTimeout(1000);
        instance = newId();
#ifdef Q_OS_WIN
        address = QStringLiteral("NovaTerm-mcp-") + instance;
#else
        address = QDir(instanceDirectory).filePath(instance + ".sock");
#endif
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { expireJobs(); });
        QObject::connect(&io, &LocalMcpServer::packet, q,
            [this](const QString& id, const QJsonObject& packet) { receive(id, packet); });
        QObject::connect(&io, &LocalMcpServer::disconnected, q,
            [this](const QString& id) { disconnect(id); });
        QObject::connect(&io, &LocalMcpServer::listening, q, [this](bool ok) {
            listening = ok;
            if (ok) {
                ok = writePrivateJson(manifestPath(), {{"instanceId", instance},
                    {"pid", QString::number(QCoreApplication::applicationPid())},
                    {"startedAt", utcNow()}, {"applicationVersion", NOVATERM_VERSION},
                    {"ipcVersion", 1}, {"endpoint", address}});
            }
            status = ok ? QStringLiteral("Listening") : QStringLiteral("IPC startup failed");
            if (!ok) { listening = false; io.stop(); }
            emit q->changed();
        });
        QObject::connect(&access, &AccessStore::changed, q, [this] { updateEnabled(); emit q->changed(); });
        QObject::connect(&access, &AccessStore::revoked, q,
            [this](const QString& client, const QString& session) { revoke(client, session); });
        QObject::connect(&sessions, &SessionDirectory::changed, q, &Service::changed);
        QObject::connect(&sessions, &SessionDirectory::invalidated, q, [this](const QString& id) {
            lastCapture.remove(id);
            lastRevision.remove(id);
            lastBase.remove(id);
            publishedStats.remove(id);
            const auto jobsCopy = jobs;
            for (const auto& job : jobsCopy) {
                if (job->arguments.value("sessionId").toString() == id)
                    cancel(job, error("STALE_SESSION_EPOCH"), true);
            }
            for (auto it = captures.begin(); it != captures.end();) {
                if (it->session == id) it = captures.erase(it); else ++it;
            }
        });
        updateEnabled();
    }

    QString manifestPath() const { return QDir(instanceDirectory).filePath(instance + ".json"); }
    void updateEnabled()
    {
        if (!access.enabled()) { stop(); status = "Disabled"; return; }
        if (!started) {
            if (!secureDirectory(instanceDirectory)) { status = "Cannot secure IPC directory"; return; }
            started = true;
            status = "Starting";
            io.start(address);
        }
    }
    void stop()
    {
        const auto connectionIds = connections.keys();
        for (const auto& id : connectionIds)
            disconnect(id);
        for (auto& execution : executions) {
            if (!execution.complete) {
                if (execution.coordinator)
                    execution.coordinator->cancel(execution.executorRequestId);
                else if (execution.facade)
                    execution.facade->cancel(execution.executorRequestId);
                CommandExecutionResult result;
                result.requestId = execution.executorRequestId;
                result.executionMayHaveStarted = true;
                result.outcome = CommandExecutionOutcome::Disconnected;
                finishExecution(result);
            }
        }
        io.stop();
        QFile::remove(manifestPath());
        timer.stop();
        started = listening = false;
        workers.waitForDone();
    }

    void receive(const QString& connectionId, const QJsonObject& message)
    {
        const auto op = message.value("op").toString();
        if (op == "hello" && !connections.contains(connectionId)) {
            const auto token = message.value("token").toString().toLatin1();
            const auto client = access.authenticate(token);
            if (client.isEmpty() || message.value("instanceId") != instance
                || message.value("ipcVersion").toInt() != 1 || connections.size() >= MaxClients) {
                if (!io.send(connectionId, {{"op", "hello"}, {"ok", false}}))
                    io.closeConnection(connectionId);
                return;
            }
            Connection newConnection;
            newConnection.client = client;
            newConnection.key = randomBytes();
            connections.insert(connectionId, std::move(newConnection));
            if (!io.send(connectionId, {{"op", "hello"}, {"ok", true}, {"instanceId", instance}})) {
                disconnect(connectionId);
                return;
            }
            emit q->changed();
            return;
        }
        auto connection = connections.find(connectionId);
        if (connection == connections.end()) { io.closeConnection(connectionId); return; }
        const auto id = message.value("id").toString();
        if (id.isEmpty() || id.size() > 64) { io.closeConnection(connectionId); return; }
        if (op == "elicitation_result") {
            const auto key = jobKey(connectionId, id);
            auto pending = confirmations.find(key);
            if (pending == confirmations.end()) return;
            const auto confirmation = pending.value();
            confirmations.erase(pending);
            const QString action = message.value("action").toString();
            if (action != "accept" || !message.value("confirmed").toBool()) {
                complete(confirmation.job, error(action == "cancel"
                    ? "COMMAND_CONFIRMATION_CANCELLED" : "COMMAND_CONFIRMATION_DECLINED"));
                return;
            }
            const auto entry = sessions.find(confirmation.session);
            const auto currentConnection = connections.constFind(connectionId);
            auto* coordinator = entry && entry->session
                ? entry->session->commandCoordinator() : nullptr;
            auto* inputArbiter = entry && entry->session
                ? entry->session->inputArbiter() : nullptr;
            auto* facade = entry && entry->session
                ? entry->session->commandFacade() : nullptr;
            const bool unverifiedSsh = entry && entry->kind == TransportKind::Ssh
                && coordinator && coordinator->allowsUnverifiedPrompt();
            const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
            RiskAssessment currentRisk;
            QByteArray digest;
            if (confirmation.tool == QStringLiteral("novaterm_run_script")) {
                const auto& arguments = confirmation.job->arguments;
                const QByteArray content = arguments.value("scriptContent").toString().toUtf8();
                currentRisk = CommandRiskPolicy{}.classifyScript(content,
                    arguments.value("targetPath").toString(),
                    arguments.value("workingDirectory").toString(),
                    arguments.value("invocation").toString());
                digest = scriptArgumentsDigest(arguments);
            } else {
                currentRisk = CommandRiskPolicy{}.classify(confirmation.command);
                digest = QCryptographicHash::hash(confirmation.command.toUtf8(),
                    QCryptographicHash::Sha256);
            }
            if (!entry || currentConnection == connections.cend()
                || currentConnection->client != confirmation.client
                || !access.canRead(confirmation.client, *entry)
                || (confirmation.tool == QStringLiteral("novaterm_run_script")
                    && !access.canRunScriptTask(confirmation.client, *entry))
                || (confirmation.tool == QStringLiteral("novaterm_run_command")
                    && !access.canRunCommand(confirmation.client, *entry))
                || entry->epoch != confirmation.epoch || !coordinator
                || (!unverifiedSsh && !coordinator->isPromptReady())
                || (unverifiedSsh && (!inputArbiter
                    || inputArbiter->userInputGeneration()
                        != confirmation.userInputGeneration))
                || !facade || profile.version() != confirmation.profileVersion
                || entry->targetFingerprint != confirmation.targetFingerprint
                || access.version(confirmation.client) != confirmation.grantVersion
                || coordinator->promptGeneration() != confirmation.promptGeneration
                || digest != confirmation.payloadHash
                || currentRisk.policyVersion != confirmation.policyVersion
                || (currentRisk.decision != RiskDecision::Confirm
                    && currentRisk.decision != RiskDecision::Unknown)
                || (confirmation.tool == QStringLiteral("novaterm_run_script")
                    && (!entry->session->scriptProvider()
                        || !entry->session->scriptProvider()->isAvailable()))) {
                complete(confirmation.job, error("COMMAND_CONFIRMATION_STALE"));
                return;
            }
            confirmation.job->confirmationAccepted = true;
            confirmation.job->deadline = clock.elapsed() + 60000;
            if (confirmation.tool == QStringLiteral("novaterm_run_script"))
                runScript(confirmation.job, *entry);
            else
                runCommand(confirmation.job, *entry);
            return;
        }
        if (op == "cancel") {
            const auto job = jobs.value(jobKey(connectionId, id));
            if (job) cancel(job, {}, false);
            return;
        }
        if (op != "call" || !message.value("arguments").isObject()
            || jobs.contains(jobKey(connectionId, id))) {
            io.closeConnection(connectionId);
            return;
        }
        const auto name = message.value("tool").toString();
        ++requestCount;
        const auto arguments = message.value("arguments").toObject();
        const auto invalid = validateArguments(name, arguments);
        if (!invalid.isEmpty()) {
            ++rejectedCount;
            send(connectionId, id, failure("INVALID_ARGUMENT", invalid));
            return;
        }
        if (connection->waiting.size() >= MaxQueuedRequests) {
            ++rejectedCount;
            send(connectionId, id, error("BUSY", true, 250));
            return;
        }
        auto job = std::make_shared<Job>();
        job->connection = connectionId;
        job->id = id;
        job->tool = name;
        job->arguments = arguments;
        job->mrtr = message.value("mrtr").toBool();
        job->formElicitation = message.value("formElicitation").toBool();
        job->requestState = message.value("requestState").toString();
        job->inputResponses = message.value("inputResponses").toObject();
        if (job->requestState.size() > 4096 || (!job->mrtr
            && (!job->requestState.isEmpty() || !job->inputResponses.isEmpty()))) {
            send(connectionId, id, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        job->deadline = clock.elapsed() + (name == "novaterm_run_script" ? 120000
            : name == "novaterm_run_command" ? 60000 : 2000);
        job->created = clock.elapsed();
        jobs.insert(jobKey(connectionId, id), job);
        peakRequests = std::max(peakRequests, jobs.size());
        connection->waiting.push_back(job);
        timer.setInterval(100);
        timer.start();
        schedule();
    }

    void schedule()
    {
        if (scheduled)
            return;
        scheduled = true;
        QTimer::singleShot(0, q, [this] {
            scheduled = false;
            const auto ids = connections.keys();
            for (const auto& id : ids) {
                auto it = connections.find(id);
                if (it == connections.end() || it->active >= MaxActiveRequests || it->waiting.empty())
                    continue;
                auto job = it->waiting.front();
                it->waiting.pop_front();
                if (job->cancelled)
                    continue;
                ++it->active;
                job->running = true;
                execute(job);
            }
            for (const auto& connection : connections) {
                if (connection.active < MaxActiveRequests && !connection.waiting.empty()) {
                    schedule();
                    break;
                }
            }
        });
    }

    void send(const QString& connection, const QString& id, QJsonObject payload)
    {
        QJsonObject packet{{"op", "result"}, {"id", id}, {"payload", payload}};
        if (frame(packet).isEmpty())
            packet.insert("payload", error("RESPONSE_TOO_LARGE"));
        if (!io.send(connection, packet)) {
            io.closeConnection(connection);
            disconnect(connection);
        }
    }
    void registerActiveConfirmation(const QString& connection,
        const QString& client, const QString& session, const QString& nonce,
        qint64 expiry)
    {
        activeConfirmations.insert(connection, nonce);
        confirmationExpiries.insert(nonce, expiry);
        activeConfirmationScopes.insert(nonce,
            ConfirmationScope{connection, client, session, expiry});
    }
    void clearActiveConfirmation(const QString& connection,
                                 const QString& nonce)
    {
        if (activeConfirmations.value(connection) == nonce)
            activeConfirmations.remove(connection);
        confirmationExpiries.remove(nonce);
        activeConfirmationScopes.remove(nonce);
    }
    void consumeActiveConfirmation(const QString& connection,
                                   const QString& nonce, qint64 expiry)
    {
        consumedConfirmations.insert(nonce, expiry);
        clearActiveConfirmation(connection, nonce);
    }
    void complete(const std::shared_ptr<Job>& job, QJsonObject payload)
    {
        if (!jobs.contains(jobKey(job->connection, job->id)))
            return;
        confirmations.remove(jobKey(job->connection, job->id));
        const QString owner = jobKey(job->connection, job->id);
        for (auto it = scriptPathLocks.begin(); it != scriptPathLocks.end();) {
            if (it.value() == owner) it = scriptPathLocks.erase(it);
            else ++it;
        }
        auto connection = connections.find(job->connection);
        if (connection != connections.end()) {
            const auto client = connection->client;
            if (!job->cancelled) {
                const auto sessionId = job->arguments.value("sessionId").toString();
                if (!sessionId.isEmpty()) {
                    const auto entry = sessions.find(sessionId);
                    if (!jobs.contains(jobKey(job->connection, job->id)))
                        return;
                    if (!entry || !access.canRead(client, *entry))
                        payload = error("SESSION_NOT_AVAILABLE");
                    else if (entry->epoch != job->arguments.value("epoch").toString())
                        payload = error("STALE_SESSION_EPOCH");
                }
                // find() 可能同步发布会话失效，失效处理已经完成响应时不得再发送。
            }
            connection = connections.find(job->connection);
            if (connection != connections.end() && job->running)
                connection->active = std::max(0, connection->active - 1);
        }
        jobs.remove(jobKey(job->connection, job->id));
        ++completedCount;
        maximumRequestMs = std::max(maximumRequestMs, clock.elapsed() - job->created);
        if (!job->cancelled && connections.contains(job->connection))
            send(job->connection, job->id, payload);
        if (jobs.isEmpty()) {
            if (captures.isEmpty() && executions.isEmpty()
                && lastCommand.isEmpty() && confirmations.isEmpty()
                && activeConfirmations.isEmpty() && consumedConfirmations.isEmpty()) {
                timer.stop();
            } else {
                timer.setInterval(1000);
                timer.start();
            }
        }
        schedule();
    }
    void cancel(const std::shared_ptr<Job>& job, QJsonObject payload, bool reply)
    {
        if (job->cancelled.exchange(true))
            return;
        ++cancelledCount;
        QList<QPair<QPointer<ISessionScriptProvider>, quint64>> scriptCancels;
        for (auto it = scriptWrites.begin(); it != scriptWrites.end();) {
            if (it->job == job) {
                scriptCancels.append({it->provider, it.key()});
                it = scriptWrites.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& scriptCancel : scriptCancels) {
            if (scriptCancel.first)
                scriptCancel.first->cancelWrite(scriptCancel.second);
        }
        QList<quint64> unresolvedRequests;
        for (auto& execution : executions) {
            if (execution.complete || execution.job != job)
                continue;
            if (execution.coordinator)
                execution.coordinator->cancel(execution.executorRequestId);
            else if (execution.facade)
                execution.facade->cancel(execution.executorRequestId);
            // cancel() 允许同步发布确定结果；只有仍未完成的执行才合成保守终态。
            if (!execution.complete)
                unresolvedRequests.append(execution.executorRequestId);
        }
        for (const quint64 requestId : unresolvedRequests) {
            // SSH 队列取消会 queued 返回“未开始”的确定证据；Local helper 也可能
            // 在 terminate/kill 后很快给出 finished。短暂等待这些更强证据，只有
            // Executor 没有回报时才合成保守的 unknown 终态。
            QTimer::singleShot(CancelCompletionGraceMs, q, [this, requestId] {
                const auto execution = std::find_if(
                    executions.cbegin(), executions.cend(),
                    [requestId](const Execution& value) {
                        return value.executorRequestId == requestId;
                    });
                if (execution == executions.cend() || execution->complete)
                    return;
                CommandExecutionResult result;
                result.requestId = requestId;
                result.outcome = CommandExecutionOutcome::Cancelled;
                result.executionMayHaveStarted = true;
                result.terminationConfirmed = false;
                finishExecution(result);
            });
        }
        if (reply && connections.contains(job->connection))
            send(job->connection, job->id, payload);
        complete(job, {});
    }
    void disconnect(const QString& connection)
    {
        const QString nonce = activeConfirmations.take(connection);
        confirmationExpiries.remove(nonce);
        activeConfirmationScopes.remove(nonce);
        const auto copy = jobs;
        for (const auto& job : copy) {
            if (job->connection == connection)
                cancel(job, {}, false);
        }
        connections.remove(connection);
        for (auto it = captures.begin(); it != captures.end();) {
            if (it->connection == connection) it = captures.erase(it); else ++it;
        }
        emit q->changed();
    }
    void revoke(const QString& client, const QString& session)
    {
        for (auto it = activeConfirmationScopes.begin();
             it != activeConfirmationScopes.end();) {
            const bool matchesClient = client.isEmpty() || it->client == client;
            const bool matchesSession = session.isEmpty() || it->session == session;
            if (matchesClient && matchesSession) {
                if (activeConfirmations.value(it->connection) == it.key())
                    activeConfirmations.remove(it->connection);
                confirmationExpiries.remove(it.key());
                it = activeConfirmationScopes.erase(it);
            } else {
                ++it;
            }
        }
        const auto ids = connections.keys();
        for (const auto& id : ids) {
            if (!client.isEmpty() && connections.value(id).client != client)
                continue;
            if (session.isEmpty()) {
                io.closeConnection(id);
                disconnect(id);
            } else {
                const auto copy = jobs;
                for (const auto& job : copy) {
                    if (job->connection == id && job->arguments.value("sessionId").toString() == session)
                        cancel(job, error("SESSION_NOT_AVAILABLE"), true);
                }
                for (auto it = captures.begin(); it != captures.end();) {
                    if (it->connection == id && it->session == session) it = captures.erase(it); else ++it;
                }
            }
        }
    }
    void expireJobs()
    {
        const auto copy = jobs;
        for (const auto& job : copy) {
            if (job->deadline > clock.elapsed()) continue;
            bool commandExpired = false;
            for (const auto& execution : executions) {
                if (!execution.complete && execution.job == job) {
                    if (execution.coordinator)
                        execution.coordinator->expire(execution.executorRequestId);
                    else if (execution.facade)
                        execution.facade->cancel(execution.executorRequestId);
                    CommandExecutionResult result;
                    result.requestId = execution.executorRequestId;
                    result.executionMayHaveStarted = true;
                    result.outcome = CommandExecutionOutcome::TimedOut;
                    finishExecution(result);
                    commandExpired = true;
                    break;
                }
            }
            if (!commandExpired) cancel(job, error("DEADLINE_EXCEEDED"), true);
        }
        cleanup();
        if (jobs.isEmpty()) {
            if (captures.isEmpty() && executions.isEmpty()
                && lastCommand.isEmpty() && confirmations.isEmpty()
                && activeConfirmations.isEmpty() && consumedConfirmations.isEmpty()) timer.stop();
            else timer.setInterval(1000);
        }
    }
    void cleanup()
    {
        const auto now = clock.elapsed();
        for (auto it = consumedConfirmations.begin(); it != consumedConfirmations.end();) {
            if (it.value() <= now) it = consumedConfirmations.erase(it);
            else ++it;
        }
        for (auto it = activeConfirmations.begin(); it != activeConfirmations.end();) {
            const QString nonce = it.value();
            const auto expiry = confirmationExpiries.value(nonce, 0);
            if (expiry <= now) {
                confirmationExpiries.remove(nonce);
                activeConfirmationScopes.remove(nonce);
                it = activeConfirmations.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = confirmations.begin(); it != confirmations.end();) {
            if (it->expires <= now) {
                const auto job = it->job;
                it = confirmations.erase(it);
                complete(job, error("COMMAND_CONFIRMATION_EXPIRED"));
            } else {
                ++it;
            }
        }
        for (auto it = captures.begin(); it != captures.end();) {
            if (it->expires <= now) it = captures.erase(it); else ++it;
        }
        for (auto it = executions.begin(); it != executions.end();) {
            if (it->complete && now - it->submitted >= 600000) it = executions.erase(it); else ++it;
        }
        for (auto it = lastCommand.begin(); it != lastCommand.end();) {
            if (now - *it >= 5000) it = lastCommand.erase(it); else ++it;
        }
    }

    void execute(const std::shared_ptr<Job>& job)
    {
        cleanup();
        if (job->deadline <= clock.elapsed()) { complete(job, error("DEADLINE_EXCEEDED")); return; }
        if ((!job->requestState.isEmpty() || !job->inputResponses.isEmpty())
            && job->tool != "novaterm_run_command"
            && job->tool != "novaterm_run_script") {
            complete(job, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        const auto connection = connections.value(job->connection);
        if (!access.enabled()) { complete(job, error("MCP_DISABLED")); return; }
        if (job->tool == "novaterm_list_sessions") { listSessions(job); return; }
        const auto entry = sessions.find(job->arguments.value("sessionId").toString());
        if (job->cancelled) return;
        if (!entry || !access.canRead(connection.client, *entry)) {
            complete(job, !job->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("SESSION_NOT_AVAILABLE"));
            return;
        }
        if (entry->epoch != job->arguments.value("epoch").toString()) {
            complete(job, !job->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("STALE_SESSION_EPOCH"));
            return;
        }
        if (job->tool == "novaterm_list_commands") { listCommands(job, *entry); return; }
        if (job->tool == "novaterm_execute_command") { command(job, *entry); return; }
        if (job->tool == "novaterm_run_command") { runCommand(job, *entry); return; }
        if (job->tool == "novaterm_run_script") { runScript(job, *entry); return; }
        if (entry->state == SessionState::Closing || entry->state == SessionState::Closed) {
            complete(job, error("SESSION_CLOSED")); return;
        }
        if (entry->state != SessionState::Running && entry->state != SessionState::Failed) {
            complete(job, error("SESSION_NOT_READY")); return;
        }
        if (job->tool == "novaterm_read_context") readContext(job, *entry);
        else search(job, *entry);
    }

    void listSessions(const std::shared_ptr<Job>& job)
    {
        const auto connection = connections.value(job->connection);
        QString after;
        const auto cursor = job->arguments.value("cursor").toString();
        if (!cursor.isEmpty()) {
            const auto token = verifyToken(cursor, connection.key);
            if (!token || token->value("kind") != "directory"
                || token->value("version").toString() != versionString(sessions.revision())
                || token->value("grant").toString() != versionString(access.version(connection.client))) {
                complete(job, error("CURSOR_STALE")); return;
            }
            after = token->value("after").toString();
        }
        const int limit = job->arguments.value("limit").toInt(50);
        const auto versionBeforePage = sessions.revision();
        const auto all = sessions.pageAfter(after, limit + 1, [&](const SessionDirectory::Entry& entry) {
            return access.canRead(connection.client, entry) && entry.state != SessionState::Closing
                && entry.state != SessionState::Closed;
        });
        if (!cursor.isEmpty() && versionBeforePage != sessions.revision()) {
            complete(job, error("CURSOR_STALE")); return;
        }
        QJsonArray rows;
        QString last;
        bool more = false;
        for (const auto& entry : all) {
            if (entry.id <= after || !access.canRead(connection.client, entry)
                || entry.state == SessionState::Closing || entry.state == SessionState::Closed)
                continue;
            if (rows.size() >= limit) { more = true; break; }
            bool truncated = false;
            const auto title = boundedText(entry.title, 256, &truncated);
            QJsonArray capabilities{"read_context", "search_context", "list_commands"};
            if (job->formElicitation)
                capabilities.append("human_confirmation");
            auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
            const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
            const auto granted = access.commands(connection.client, entry);
            bool canExecute = facade && facade->isAvailable()
                && profile.isAvailable();
            if (canExecute) {
                canExecute = std::any_of(granted.cbegin(), granted.cend(),
                    [&profile](const QString& commandId) {
                        return profile.supports(commandId);
                    });
            }
            if (canExecute)
                capabilities.append("execute_command");
            auto* coordinator = entry.session
                ? entry.session->commandCoordinator() : nullptr;
            const bool unverifiedSsh = entry.kind == TransportKind::Ssh
                && coordinator && coordinator->allowsUnverifiedPrompt();
            // 两项能力都以「交互通路可达」为前提，但授权位各自独立：
            // run_command 看 canRunCommand，run_script 看 canRunScriptTask。
            const bool interactiveReachable = facade && facade->isAvailable()
                && !entry.targetFingerprint.isEmpty()
                && (facade->capabilities().mode
                        == CommandExecutionMode::InteractiveFramed
                    || unverifiedSsh);
            if (interactiveReachable
                && access.canRunCommand(connection.client, entry)) {
                capabilities.append("run_command");
            }
            if (interactiveReachable && !unverifiedSsh
                && access.canRunScriptTask(connection.client, entry)
                && entry.session->scriptProvider()
                && entry.session->scriptProvider()->isAvailable()) {
                capabilities.append("run_script");
            }
            rows.append(QJsonObject{{"sessionId", entry.id}, {"epoch", entry.epoch},
                {"state", SessionDirectory::stateName(entry.state)}, {"transport", SessionDirectory::transportName(entry.kind)},
                {"displayName", title}, {"displayNameTruncated", truncated}, {"capabilities", capabilities}});
            last = entry.id;
        }
        QJsonValue next(QJsonValue::Null);
        if (more) next = signToken({{"kind", "directory"}, {"after", last},
            {"version", versionString(sessions.revision())}, {"grant", versionString(access.version(connection.client))}}, connection.key);
        complete(job, success({{"instanceId", instance}, {"applicationVersion", NOVATERM_VERSION},
            {"sessions", rows}, {"nextCursor", next}}));
    }

    void readContext(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        QElapsedTimer captureTimer;
        captureTimer.start();
        const auto connection = connections.value(job->connection);
        quint64 since = 0;
        const bool viewport = job->arguments.value("includeViewport").toBool(true);
        const auto encoded = job->arguments.value("sinceToken").toString();
        if (!encoded.isEmpty()) {
            const auto token = verifyToken(encoded, connection.key);
            bool ok = false;
            if (token) since = token->value("revision").toString().toULongLong(&ok);
            if (!token || !ok || token->value("kind") != "context" || token->value("session") != entry.id
                || token->value("epoch") != entry.epoch || token->value("viewport").toBool() != viewport
                || token->value("grant").toString() != versionString(access.version(connection.client))) {
                complete(job, error("CURSOR_STALE")); return;
            }
        }
        const auto now = clock.elapsed();
        const bool usePublished = NovaTerm::publishedContextSnapshotEnabled();
        std::shared_ptr<const TerminalContextProvider::Snapshot> base;
        if (lastCapture.contains(entry.id) && now - lastCapture.value(entry.id) < 250) {
            if (!usePublished) {
                // 仅旧 try-read 路径需要严格 revision 校验：合并窗内模型已推进就
                // 立即 Busy，不把陈旧结果当成新鲜捕获。发布路径按设计允许有界陈旧
                // （保留原 capturedAt），因此不做这项拒绝。
                const auto current = entry.session->core()->tryModelRevision();
                if (!current || lastRevision.value(entry.id) != *current) {
                    complete(job, error("BUSY", true, 250)); return;
                }
            }
            ++reusedCaptureCount;
            ++coalescedReadCount;
            base = lastBase.value(entry.id);
        }
        if (!base) {
            // 每次真正向 Provider 取一份新基础摘要才计数；合并窗内复用不重复计。
            // 与 snapshotPublishCount（Parser 侧发布次数，≤4 Hz）是两个不同口径，
            // 二者比值即「客户端读取需求被合并/发布吸收了多少」。
            ++coreCaptureCount;
            base = entry.session->tryTerminalContext();
            if (usePublished) {
                const auto currentStats = entry.session->core()
                    ->publishedContextStatistics();
                const auto previousStats = publishedStats.value(entry.id);
                snapshotPublishCount += currentStats.publishCount
                    - previousStats.publishCount;
                snapshotReuseCount += currentStats.reuseCount
                    - previousStats.reuseCount;
                publishedStats.insert(entry.id, currentStats);
            }
            if (base) {
                lastCapture.insert(entry.id, now);
                lastRevision.insert(entry.id, base->state.revision);
                lastBase.insert(entry.id, base);
            }
        }
        if (!base) { complete(job, error("BUSY", true, 50)); return; }
        const bool sourceTruncated = base->state.truncated;
        bool outputTruncated = false;
        bool reset = sourceTruncated || (!encoded.isEmpty() && (since < base->resetRevision
            || since > base->state.revision || (base->cacheFloor > 0 && since <= base->cacheFloor)));
        qsizetype remaining = job->arguments.value("maxBytes").toInt(65536);
        int remainingLines = job->arguments.value("maxLines").toInt(256);
        const auto text = [&](const std::string& string) {
            const auto bytes = QByteArray::fromStdString(string);
            const auto clipped = clipUtf8(bytes, remaining);
            outputTruncated |= clipped.size() != bytes.size();
            remaining -= clipped.size();
            projectionCopiedBytes += quint64(clipped.size());
            return QString::fromUtf8(clipped);
        };
        const auto title = text(base->state.title);
        QJsonArray screen, recent;
        const auto line = [&](QJsonArray& array, const std::string& value) {
            if (remainingLines == 0 || remaining == 0) { outputTruncated = true; return; }
            array.append(QJsonObject{{"text", text(value)}});
            --remainingLines;
        };
        if (viewport) for (const auto& value : base->state.viewport) line(screen, value.text);
        if (!base->state.alternateScreen) {
            for (const auto& value : base->entries) {
                if (encoded.isEmpty() || reset || value.revision > since) line(recent, value.text);
            }
        }
        reset |= outputTruncated;
        const auto captureId = newId();
        QJsonValue next(QJsonValue::Null);
        if (!sourceTruncated && !outputTruncated)
            next = signToken({{"kind", "context"}, {"session", entry.id}, {"epoch", entry.epoch},
                {"revision", versionString(base->state.revision)}, {"grant", versionString(access.version(connection.client))},
                {"viewport", viewport}}, connection.key);
        const auto capturedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            base->capturedAt.time_since_epoch()).count();
        const qint64 snapshotAge = std::max<qint64>(
            0, QDateTime::currentMSecsSinceEpoch() - capturedMs);
        if (snapshotAges.size() == 256) snapshotAges.pop_front();
        snapshotAges.push_back(snapshotAge);
        const QString capturedAt = QDateTime::fromMSecsSinceEpoch(
            capturedMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
        QJsonObject data{{"instanceId", instance}, {"sessionId", entry.id}, {"epoch", entry.epoch},
            {"revision", versionString(base->state.revision)}, {"captureId", captureId}, {"capturedAt", capturedAt},
            {"state", SessionDirectory::stateName(entry.state)}, {"transport", SessionDirectory::transportName(entry.kind)},
            {"title", title}, {"alternateScreen", base->state.alternateScreen},
            {"cursor", QJsonObject{{"row", base->state.cursor.position.row}, {"column", base->state.cursor.position.col}}},
            {"viewport", screen}, {"recentOutput", recent}, {"coverage", "bounded_summary"},
            {"historyMayPrecedeEpoch", true}, {"sourceTruncated", sourceTruncated}, {"outputTruncated", outputTruncated},
            {"resetRequired", reset}, {"suppressedDuplicates", versionString(base->suppressedDuplicates)}, {"nextToken", next}};
        QList<QString> owned;
        for (auto it = captures.cbegin(); it != captures.cend(); ++it)
            if (it->connection == job->connection) owned.append(it.key());
        if (owned.size() >= MaxCaptures) {
            const auto oldest = *std::min_element(owned.begin(), owned.end(), [this](const QString& a, const QString& b) {
                return captures.value(a).created < captures.value(b).created;
            });
            captures.remove(oldest);
        }
        captures.insert(captureId, Capture{job->connection, entry.id,
            entry.epoch, data.value("revision").toString(), screen, recent,
            base, sourceTruncated, outputTruncated,
            now + CaptureLifetimeMs, now});
        complete(job, success(data));
        if (captureDurations.size() == 256) captureDurations.pop_front();
        captureDurations.push_back(captureTimer.nsecsElapsed());
    }

    void search(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto id = job->arguments.value("captureId").toString();
        const auto capture = captures.constFind(id);
        if (capture == captures.cend() || capture->connection != job->connection
            || capture->session != entry.id || capture->epoch != entry.epoch) {
            complete(job, error("CAPTURE_NOT_AVAILABLE")); return;
        }
        const QJsonObject data{{"revision", capture->revision},
            {"viewport", capture->viewport},
            {"recentOutput", capture->recentOutput},
            {"sourceTruncated", capture->sourceTruncated},
            {"outputTruncated", capture->outputTruncated}};
        QPointer<Service> guard(q);
        workers.start(QRunnable::create([this, guard, job, data, id] {
            NovaTerm::setCurrentThreadName("nvterm-mcp-srch");
            QElapsedTimer timer;
            timer.start();
            const bool sensitive = job->arguments.value("caseSensitive").toBool(true);
            const auto fold = [sensitive](QByteArray bytes) {
                if (!sensitive) for (char& c : bytes) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
                return bytes;
            };
            const auto query = fold(job->arguments.value("query").toString().toUtf8());
            const auto scope = job->arguments.value("scope").toString("both");
            const int limit = job->arguments.value("maxMatches").toInt(20);
            QJsonArray matches;
            bool limited = false;
            bool timedOut = false;
            for (const QString& source : {QStringLiteral("viewport"), QStringLiteral("recentOutput")}) {
                const auto label = source == "viewport" ? QStringLiteral("viewport") : QStringLiteral("recent_output");
                if (scope != "both" && scope != label) continue;
                const auto lines = data.value(source).toArray();
                for (qsizetype lineIndex = 0; lineIndex < lines.size() && !limited; ++lineIndex) {
                    if (job->cancelled || timer.elapsed() > 100) { timedOut = true; break; }
                    const auto bytes = fold(lines[lineIndex].toObject().value("text").toString().toUtf8());
                    qsizetype from = 0;
                    while (from <= bytes.size()) {
                        if (job->cancelled || timer.elapsed() > 100) { timedOut = true; break; }
                        // 每 4 KiB 检查取消；保留 query 长度的重叠，避免漏掉块边界命中。
                        const auto span = std::min(qsizetype(4096) + query.size() - 1, bytes.size() - from);
                        const auto relative = bytes.mid(from, span).indexOf(query);
                        if (relative < 0) {
                            if (span < 4096 || from + span == bytes.size()) break;
                            from += 4096;
                            continue;
                        }
                        const auto found = from + relative;
                        if (matches.size() == limit) { limited = true; break; }
                        matches.append(QJsonObject{{"source", label}, {"lineIndex", int(lineIndex)},
                            {"startByte", int(found)}, {"endByte", int(found + query.size())}});
                        from = found + query.size();
                    }
                    if (timedOut) break;
                }
                if (timedOut || limited) break;
            }
            if (job->cancelled || !guard) return;
            const auto result = timedOut ? error("DEADLINE_EXCEEDED") : success({{"captureId", id},
                {"revision", data.value("revision")}, {"matches", matches}, {"limited", limited},
                {"sourceTruncated", data.value("sourceTruncated")}, {"outputTruncated", data.value("outputTruncated")}});
            // guard 可能在上面那次检查之后、走到这里之前被销毁（Service::stop()
            // 会 workers.waitForDone()，但 queued 的 lambda 仍待投递）。此时若照样
            // 投递，Qt 会走 "Invalid nullptr argument" 警告路径且 lambda 不执行 ——
            // 没有 UAF，但那条警告在日志里与真实协议故障无法区分。显式跳过。
            if (!guard)
                return;
            QMetaObject::invokeMethod(guard, [this, guard, job, result] { if (guard) complete(job, result); }, Qt::QueuedConnection);
        }));
    }

    void listCommands(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        const auto allowed = access.commands(connection.client, entry);
        QString reason;
        auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
        const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
        const QString policyVersion = CommandPolicy::version(profile);
        if (entry.state != SessionState::Running) reason = "SESSION_NOT_READY";
        else if (!profile.isAvailable()) reason = "COMMAND_PROFILE_UNAVAILABLE";
        else if (!facade || !facade->isAvailable()) reason = "COMMAND_UNAVAILABLE";
        else if (allowed.isEmpty()) reason = "COMMAND_PERMISSION_REQUIRED";
        else if (entry.targetFingerprint.isEmpty()) reason = "COMMAND_UNAVAILABLE";
        QJsonArray catalog;
        if (reason.isEmpty()) for (const auto& command : CommandPolicy::catalog(profile)) {
            if (!allowed.contains(command.id)) continue;
            const auto ticket = signToken({{"kind", "command"}, {"execution", newId()},
                {"session", entry.id}, {"epoch", entry.epoch}, {"command", command.id},
                {"policy", policyVersion}, {"profile", profile.version()},
                {"grant", versionString(access.version(connection.client))},
                {"expires", clock.elapsed() + 60000}}, connection.key);
            const QString preview = entry.kind == TransportKind::LocalShell
                && facade->capabilities().mode == CommandExecutionMode::Isolated
                ? QStringLiteral("novaterm-local-diag ") + command.id
                : QString::fromUtf8(command.command);
            catalog.append(QJsonObject{{"commandId", command.id}, {"title", command.title},
                {"preview", preview},
                {"argumentSchema", QJsonObject{{"type", "object"}, {"additionalProperties", false}}},
                {"timeoutMs", 5000}, {"maxOutputBytes", CommandOutputBytes}, {"commandTicket", ticket}});
        }
        complete(job, success({{"instanceId", instance}, {"sessionId", entry.id}, {"epoch", entry.epoch},
            {"policyVersion", policyVersion}, {"executionEnabled", reason.isEmpty()},
            {"disabledReason", reason.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(reason)}, {"commands", catalog}}));
    }

    void runScript(const std::shared_ptr<Job>& job,
                   const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        if (!access.canRunScriptTask(connection.client, entry)) {
            complete(job, !job->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("SCRIPT_PERMISSION_REQUIRED"));
            return;
        }
        if (entry.state != SessionState::Running || !entry.session) {
            complete(job, error("SESSION_NOT_READY"));
            return;
        }
        auto* provider = entry.session->scriptProvider();
        if (!provider || !provider->isAvailable()) {
            complete(job, error("SCRIPT_PROVIDER_UNAVAILABLE"));
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
            complete(job, error("COMMAND_PROFILE_UNAVAILABLE"));
            return;
        }

        const auto& arguments = job->arguments;
        const QByteArray content = arguments.value("scriptContent").toString().toUtf8();
        const QString targetPath = arguments.value("targetPath").toString();
        const QString workingDirectory = arguments.value("workingDirectory").toString();
        const QString invocation = arguments.value("invocation").toString();
        const auto risk = CommandRiskPolicy{}.classifyScript(
            content, targetPath, workingDirectory, invocation);
        if (risk.decision == RiskDecision::Deny) {
            complete(job, error("COMMAND_NOT_ALLOWED"));
            return;
        }
        if (risk.decision != RiskDecision::Confirm) {
            complete(job, error("COMMAND_SCRIPT_RISK_UNKNOWN"));
            return;
        }
        const QString displayTarget = normalizedScriptTarget(
            entry.kind, workingDirectory, targetPath);
        if (displayTarget.isEmpty()) {
            complete(job, error("SCRIPT_TARGET_INVALID"));
            return;
        }
        const bool targetExistsLocally = entry.kind == TransportKind::LocalShell
            && QFileInfo(displayTarget).exists();
        if (!coordinator->isPromptReady()) {
            complete(job, error("SESSION_COMMAND_NOT_READY"));
            return;
        }
        const auto profile = facade->profile();
        const QByteArray argumentHash = scriptArgumentsDigest(arguments);

        if (!job->confirmationAccepted) {
            if (!job->formElicitation && job->requestState.isEmpty()) {
                complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
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
                        == versionString(access.version(connection.client))
                    && access.canRunScriptTask(connection.client, entry)
                    && state->value("promptGeneration").toString()
                        == QString::number(coordinator->promptGeneration())
                    && state->value("expires").toDouble() > clock.elapsed()
                    && !nonce.isEmpty()
                    && activeConfirmations.value(job->connection) == nonce
                    && !consumedConfirmations.contains(nonce);
                if (!valid) {
                    complete(job, error("COMMAND_CONFIRMATION_STALE"));
                    return;
                }
                consumeActiveConfirmation(job->connection, nonce,
                    qint64(state->value("expires").toDouble()));
                const QString action = response.value("action").toString();
                if (action != QStringLiteral("accept")
                    || !response.value("content").toObject()
                        .value("confirmed").toBool()) {
                    complete(job, error(action == QStringLiteral("cancel")
                        ? "COMMAND_CONFIRMATION_CANCELLED"
                        : "COMMAND_CONFIRMATION_DECLINED"));
                    return;
                }
                job->confirmationAccepted = true;
                job->requestState.clear();
                job->inputResponses = {};
            } else if (job->mrtr) {
                if (activeConfirmations.contains(job->connection)) {
                    complete(job, error("BUSY", true, 250));
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
                    {"grantVersion", versionString(access.version(connection.client))},
                    {"promptGeneration", QString::number(coordinator->promptGeneration())},
                    {"nonce", nonce}, {"expires", double(clock.elapsed() + 60000)}};
                const QString token = signToken(state, connection.key);
                const QString message = scriptConfirmationMessage(
                    content, displayTarget, workingDirectory, invocation,
                    risk.reasons, targetExistsLocally);
                const qint64 expires = clock.elapsed() + 60000;
                registerActiveConfirmation(job->connection, connection.client,
                                           entry.id, nonce, expires);
                if (!io.send(job->connection, {{"op", "input_required"},
                        {"id", job->id}, {"message", message}, {"requestState", token}})) {
                    clearActiveConfirmation(job->connection, nonce);
                    complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                    return;
                }
                job->cancelled = true;
                complete(job, {});
                return;
            } else {
                const QString prefix = job->connection + QLatin1Char('/');
                for (auto it = confirmations.cbegin(); it != confirmations.cend(); ++it) {
                    if (it.key().startsWith(prefix)) {
                        complete(job, error("BUSY", true, 250));
                        return;
                    }
                }
                const QString pendingKey = jobKey(job->connection, job->id);
                PendingConfirmation pending;
                pending.job = job;
                pending.client = connection.client;
                pending.session = entry.id;
                pending.epoch = entry.epoch;
                pending.command = invocation;
                pending.policyVersion = risk.policyVersion;
                pending.tool = job->tool;
                pending.profileVersion = profile.version();
                pending.targetFingerprint = entry.targetFingerprint;
                pending.grantVersion = access.version(connection.client);
                pending.promptGeneration = coordinator->promptGeneration();
                pending.payloadHash = argumentHash;
                pending.expires = clock.elapsed() + 60000;
                confirmations.insert(pendingKey, pending);
                const QString message = scriptConfirmationMessage(
                    content, displayTarget, workingDirectory, invocation,
                    risk.reasons, targetExistsLocally);
                if (!io.send(job->connection, {{"op", "elicitation"},
                        {"id", job->id}, {"message", message}})) {
                    confirmations.remove(pendingKey);
                    complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                    return;
                }
                job->deadline = clock.elapsed() + 60000;
                return;
            }
        }

        const bool readyAfterConfirmation = coordinator->isPromptReady();
        const bool stillAuthorized = access.canRunScriptTask(connection.client, entry);
        const bool sameProvider = provider == entry.session->scriptProvider();
        const bool providerAvailable = provider->isAvailable();
        if (!readyAfterConfirmation || !stillAuthorized || !sameProvider
            || !providerAvailable) {
            complete(job, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        if (guards.markers().contains(entry.targetFingerprint)) {
            complete(job, error("COMMAND_EXECUTION_QUARANTINED"));
            return;
        }
        if (lastCommand.contains(entry.targetFingerprint)) {
            complete(job, error("BUSY", true, 5000));
            return;
        }
        auto* inputArbiter = entry.session->inputArbiter();
        if (!inputArbiter) {
            complete(job, error("SESSION_COMMAND_NOT_READY"));
            return;
        }
        if (!observedInputArbiters.contains(inputArbiter)) {
            observedInputArbiters.insert(inputArbiter);
            QObject::connect(inputArbiter, &SessionInputArbiter::userInputStarted, q,
                [this, session = entry.session.data()](quint64 userGeneration) {
                    cancelScriptWritesForUserInput(session, userGeneration);
                });
            QObject::connect(inputArbiter, &QObject::destroyed, q,
                [this, inputArbiter] { observedInputArbiters.remove(inputArbiter); });
        }
        const QString pathLock = entry.targetFingerprint + QLatin1Char('/')
            + displayTarget;
        if (scriptPathLocks.contains(pathLock)) {
            complete(job, error("BUSY", true, 500));
            return;
        }
        const auto command = scriptInvocationCommand(
            entry.session->runtimeConfig().transport.value(
                QStringLiteral("interactiveShellKind")).toString(),
            workingDirectory, invocation);
        if (!command || command->toUtf8().size() > 16384) {
            complete(job, error("COMMAND_SCRIPT_INVOCATION_INVALID"));
            return;
        }
        const QString owner = jobKey(job->connection, job->id);
        const quint64 requestId = nextScriptWrite++;
        ScriptWriteTask task;
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
        task.metadataHash = scriptMetadataDigest(arguments);
        task.contentHash = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
        task.promptGeneration = coordinator->promptGeneration();
        task.grantVersion = access.version(connection.client);
        task.sessionGeneration = entry.generation;
        task.userInputGeneration = inputArbiter->userInputGeneration();
        task.timeoutMs = arguments.value("timeoutMs").toInt(5000);
        if (!observedScriptProviders.contains(provider)) {
            observedScriptProviders.insert(provider);
            QObject::connect(provider, &ISessionScriptProvider::finished, q,
                [this](const ScriptWriteResult& result) { finishScriptWrite(result); });
            QObject::connect(provider, &QObject::destroyed, q,
                [this, provider] {
                    observedScriptProviders.remove(provider);
                    const auto ids = scriptWrites.keys();
                    for (const auto id : ids) {
                        if (scriptWrites.value(id).provider.data() != provider
                            && !scriptWrites.value(id).provider.isNull())
                            continue;
                        const auto job = scriptWrites.take(id).job;
                        complete(job, error("SCRIPT_PROVIDER_UNAVAILABLE"));
                    }
                });
        }
        scriptPathLocks.insert(pathLock, owner);
        scriptWrites.insert(requestId, task);
        ScriptWriteRequest request{requestId, content, targetPath, workingDirectory};
        job->deadline = clock.elapsed() + 120000;
        if (!provider->writeScript(request)) {
            scriptWrites.remove(requestId);
            complete(job, error("SCRIPT_WRITE_FAILED"));
            return;
        }
        job->scriptPolicyVersion = risk.policyVersion;
        job->arguments.remove(QStringLiteral("scriptContent"));
    }

    void cancelScriptWritesForUserInput(TerminalSession* session,
                                        quint64 userGeneration)
    {
        if (!session)
            return;
        const QString sessionId = session->id().toString(QUuid::WithoutBraces);
        const auto ids = scriptWrites.keys();
        for (const quint64 requestId : ids) {
            const auto task = scriptWrites.value(requestId);
            if (task.session == sessionId
                && task.userInputGeneration != userGeneration) {
                cancel(task.job, error("SCRIPT_CANCELLED_BY_USER"), true);
            }
        }
    }

    void finishScriptWrite(const ScriptWriteResult& result)
    {
        const auto it = scriptWrites.find(result.requestId);
        if (it == scriptWrites.end())
            return;
        const ScriptWriteTask task = it.value();
        scriptWrites.erase(it);
        const auto job = task.job;
        if (!jobs.contains(jobKey(job->connection, job->id)))
            return;
        if (!result.success) {
            complete(job, failure(result.errorCode.isEmpty()
                ? QStringLiteral("SCRIPT_WRITE_FAILED") : result.errorCode,
                result.errorCode.isEmpty() ? QStringLiteral("SCRIPT_WRITE_FAILED") : result.errorCode));
            return;
        }
        auto connection = connections.constFind(job->connection);
        const auto entry = sessions.find(task.session);
        auto* coordinator = entry && entry->session
            ? entry->session->commandCoordinator() : nullptr;
        auto* facade = entry && entry->session
            ? entry->session->commandFacade() : nullptr;
        const auto provider = entry && entry->session
            ? entry->session->scriptProvider() : nullptr;
        const bool valid = entry && connection != connections.cend()
            && connection->client == task.client
            && access.canRunScriptTask(task.client, *entry)
            && entry->state == SessionState::Running
            && entry->epoch == task.epoch
            && entry->targetFingerprint == task.targetFingerprint
            && entry->generation == task.sessionGeneration
            && coordinator && coordinator->isPromptReady()
            && coordinator->promptGeneration() == task.promptGeneration
            && facade && facade->isAvailable()
            && facade->profile().version() == task.profileVersion
            && provider && provider == task.provider.data()
            && provider->isAvailable()
            && access.version(task.client) == task.grantVersion
            && scriptMetadataDigest(job->arguments) == task.metadataHash
            && result.contentHash == task.contentHash
            && result.resolvedTargetPath == task.targetPath;
        if (!valid) {
            complete(job, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        const auto command = scriptInvocationCommand(
            entry->session->runtimeConfig().transport.value(
                QStringLiteral("interactiveShellKind")).toString(),
            task.workingDirectory, task.invocation);
        if (!command || command->toUtf8().size() > 16384) {
            complete(job, error("COMMAND_SCRIPT_INVOCATION_INVALID"));
            return;
        }
        job->arguments.insert(QStringLiteral("command"), *command);
        job->scriptPolicyVersion = task.policyVersion;
        runCommand(job, *entry);
    }

    void runCommand(const std::shared_ptr<Job>& job,
                    const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        // 「允许交互命令」只约束 novaterm_run_command。脚本任务有自己的独立授权位
        // （scriptTask）与逐次人类确认；其调用命令虽然同样会打进当前终端，但那已经
        // 由「脚本授权 + 每次确认」显式承担。若在这里一并要求交互命令授权，
        // 「只授脚本任务」的合法配置会在脚本正文已落盘之后才失败，且报出
        // COMMAND_PERMISSION_REQUIRED 而不是 SCRIPT_PERMISSION_REQUIRED。
        // novaterm_run_script 走 runScript 入口的 canRunScriptTask 检查。
        if (job->tool == QStringLiteral("novaterm_run_command")
            && !access.canRunCommand(connection.client, entry)) {
            complete(job, !job->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("COMMAND_PERMISSION_REQUIRED"));
            return;
        }
        if (entry.state != SessionState::Running || !entry.session) {
            complete(job, error("SESSION_NOT_READY"));
            return;
        }
        const QString command = job->arguments.value("command").toString();
        const RiskAssessment risk = CommandRiskPolicy{}.classify(command);
        if (risk.decision == RiskDecision::Deny) {
            complete(job, error("COMMAND_NOT_ALLOWED"));
            return;
        }
        if (risk.decision == RiskDecision::Allow
            && (!job->requestState.isEmpty() || !job->inputResponses.isEmpty())) {
            complete(job, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        if (risk.decision != RiskDecision::Allow && !job->confirmationAccepted
            && !job->formElicitation && job->requestState.isEmpty()) {
            complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
            return;
        }
        auto* coordinator = entry.session->commandCoordinator();
        const bool unverifiedSsh = entry.kind == TransportKind::Ssh
            && coordinator && coordinator->allowsUnverifiedPrompt();
        if (!coordinator || (!coordinator->hasTrustedProfile() && !unverifiedSsh)) {
            complete(job, error("COMMAND_PROFILE_UNAVAILABLE"));
            return;
        }
        if (!unverifiedSsh && !coordinator->isPromptReady()) {
            complete(job, error("SESSION_COMMAND_NOT_READY"));
            return;
        }
        auto* facade = entry.session->commandFacade();
        // LocalShell 的固定诊断使用隔离 helper，交互命令与脚本仍由协调器
        // 写入当前终端，避免把自由命令交给只接受四个 commandId 的 helper。
        const bool directCoordinator = unverifiedSsh
            || (entry.kind == TransportKind::LocalShell && facade
                && facade->capabilities().mode == CommandExecutionMode::Isolated);
        const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
        if (risk.decision != RiskDecision::Allow && !job->confirmationAccepted) {
            if (!job->formElicitation && job->requestState.isEmpty()) {
                complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
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
                    && state->value("grantVersion") == versionString(access.version(connection.client))
                    && state->value("promptGeneration").toString()
                        == QString::number(coordinator->promptGeneration())
                    && state->value("userInputGeneration").toString()
                        == QString::number(entry.session->inputArbiter()->userInputGeneration())
                    && state->value("expires").toDouble() > clock.elapsed()
                    && activeConfirmations.value(job->connection) == nonce
                    && !nonce.isEmpty() && !consumedConfirmations.contains(nonce);
                if (!valid) {
                    complete(job, error("COMMAND_CONFIRMATION_STALE"));
                    return;
                }
                consumeActiveConfirmation(job->connection, nonce,
                    qint64(state->value("expires").toDouble()));
                if ((!unverifiedSsh && !coordinator->isPromptReady())
                    || coordinator->promptGeneration() != state->value("promptGeneration").toString().toULongLong()) {
                    complete(job, error("COMMAND_CONFIRMATION_STALE"));
                    return;
                }
                if (action != QStringLiteral("accept")
                    || !response.value("content").toObject().value("confirmed").toBool()) {
                    complete(job, error(action == QStringLiteral("cancel")
                        ? "COMMAND_CONFIRMATION_CANCELLED" : "COMMAND_CONFIRMATION_DECLINED"));
                    return;
                }
                job->confirmationAccepted = true;
            } else if (job->mrtr) {
                if (activeConfirmations.contains(job->connection)) {
                    complete(job, error("BUSY", true, 250));
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
                    {"grantVersion", versionString(access.version(connection.client))},
                    {"promptGeneration", QString::number(coordinator->promptGeneration())},
                    {"userInputGeneration", QString::number(entry.session->inputArbiter()->userInputGeneration())},
                    {"nonce", nonce}, {"expires", double(clock.elapsed() + 60000)}};
                const QString token = signToken(state, connection.key);
                const qint64 expires = clock.elapsed() + 60000;
                registerActiveConfirmation(job->connection, connection.client,
                                           entry.id, nonce, expires);
                const QString message = QStringLiteral("NovaTerm requests confirmation to run this terminal command:\n\n%1\n\nRisk: %2")
                    .arg(command, risk.reasons.join(QStringLiteral(", ")));
                if (!io.send(job->connection, {{"op", "input_required"},
                        {"id", job->id}, {"message", message}, {"requestState", token}})) {
                    clearActiveConfirmation(job->connection, nonce);
                    complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                    return;
                }
                job->cancelled = true;
                complete(job, {});
                return;
            }
            if (job->confirmationAccepted) {
                // MRTR 回合在这里重新进入与 2025 accept 相同的执行分支。
            } else {
            const QString confirmationPrefix = job->connection + QLatin1Char('/');
            for (auto it = confirmations.cbegin(); it != confirmations.cend(); ++it) {
                if (it.key().startsWith(confirmationPrefix)) {
                    complete(job, error("BUSY", true, 250));
                    return;
                }
            }
            const QString pendingKey = jobKey(job->connection, job->id);
            PendingConfirmation pending;
            pending.job = job;
            pending.client = connection.client;
            pending.session = entry.id;
            pending.epoch = entry.epoch;
            pending.command = command;
            pending.policyVersion = risk.policyVersion;
            pending.tool = job->tool;
            pending.profileVersion = profile.version();
            pending.targetFingerprint = entry.targetFingerprint;
            pending.grantVersion = access.version(connection.client);
            pending.promptGeneration = coordinator->promptGeneration();
            pending.userInputGeneration = entry.session->inputArbiter()->userInputGeneration();
            pending.payloadHash = QCryptographicHash::hash(command.toUtf8(),
                QCryptographicHash::Sha256);
            pending.expires = clock.elapsed() + 60000;
            confirmations.insert(pendingKey, pending);
            const QString message = QStringLiteral("NovaTerm requests confirmation to run this terminal command:\n\n%1\n\nRisk: %2")
                .arg(command, risk.reasons.join(QStringLiteral(", ")));
            if (!io.send(job->connection, {{"op", "elicitation"},
                    {"id", job->id}, {"message", message}})) {
                confirmations.remove(pendingKey);
                complete(job, error("CLIENT_CONFIRMATION_UNAVAILABLE"));
                return;
            }
            job->deadline = clock.elapsed() + 60000;
            return;
            }
        }
        if (!facade || !facade->isAvailable()
            || entry.targetFingerprint.isEmpty()) {
            complete(job, error("COMMAND_UNAVAILABLE"));
            return;
        }
        if (guards.markers().contains(entry.targetFingerprint)) {
            complete(job, error("COMMAND_EXECUTION_QUARANTINED"));
            return;
        }
        int clientExecutions = 0;
        for (const auto& execution : executions) {
            if (execution.connection == job->connection)
                ++clientExecutions;
        }
        if (clientExecutions >= 32 || executions.size() >= 128
            || lastCommand.contains(entry.targetFingerprint)) {
            complete(job, error("BUSY", true, 5000));
            return;
        }
        const QString executionId = newId();
        QString rejected;
        if (!guards.reserve(entry.targetFingerprint, executionId,
                            instance, rejected)) {
            complete(job, error("BUSY", true, 250));
            return;
        }
        if (!observedFacades.contains(facade)) {
            observedFacades.insert(facade);
            QObject::connect(facade, &SessionCommandFacade::finished, q,
                [this](const CommandExecutionResult& result) {
                    finishExecution(result);
                });
            QObject::connect(facade, &QObject::destroyed, q,
                [this, facade] { observedFacades.remove(facade); });
        }
        const quint64 requestId = nextCommandRequest++;
        const int timeoutMs = job->arguments.value("timeoutMs").toInt(5000);
        const QString executionCommandId = job->tool == QStringLiteral("novaterm_run_script")
            ? QStringLiteral("interactive.script")
            : QStringLiteral("interactive.free");
        const QString executionPolicyVersion = job->tool == QStringLiteral("novaterm_run_script")
            && !job->scriptPolicyVersion.isEmpty()
            ? job->scriptPolicyVersion : risk.policyVersion;
        Execution execution{executionId, job->connection, connection.client,
            entry.id, entry.epoch, executionCommandId,
            executionPolicyVersion, entry.targetFingerprint, requestId,
            clock.elapsed(), facade, job, {}, false, true, {}, {}};
        executions.insert(executionId, execution);
        if (directCoordinator) {
            executions[executionId].coordinator = coordinator;
            if (!observedCoordinators.contains(coordinator)) {
                observedCoordinators.insert(coordinator);
                QObject::connect(coordinator, &SessionCommandCoordinator::finished, q,
                    [this](const CommandExecutionResult& result) {
                        finishExecution(result);
                    });
                QObject::connect(coordinator, &QObject::destroyed, q,
                    [this, coordinator] { observedCoordinators.remove(coordinator); });
            }
        }
        CommandExecutionRequest request;
        request.requestId = requestId;
        request.command = command.toUtf8();
        request.limits = {CommandOutputBytes, timeoutMs};
        request.commandId = executionCommandId;
        request.executionNonce = newId().toUtf8();
        request.expectedPromptGeneration = unverifiedSsh
            ? 0 : coordinator->promptGeneration();
        const bool submitted = directCoordinator
            ? coordinator->submit(request) : facade->execute(request);
        if (!submitted) {
            executions.remove(executionId);
            guards.release(entry.targetFingerprint, executionId);
            complete(job, error("SESSION_COMMAND_BUSY"));
            return;
        }
        lastCommand.insert(entry.targetFingerprint, clock.elapsed());
        job->deadline = clock.elapsed() + timeoutMs;
        emit q->changed();
    }

    void command(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        const auto commandId = job->arguments.value("commandId").toString();
        auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
        const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
        const auto definition = CommandPolicy::find(commandId, profile);
        if (!definition) { complete(job, error("COMMAND_NOT_ALLOWED")); return; }
        if (!access.commands(connection.client, entry).contains(commandId)) {
            complete(job, error("COMMAND_PERMISSION_REQUIRED")); return;
        }
        const auto ticket = verifyToken(job->arguments.value("commandTicket").toString(), connection.key);
        if (!ticket || ticket->value("kind") != "command" || ticket->value("session") != entry.id
            || ticket->value("epoch") != entry.epoch || ticket->value("command") != commandId
            || ticket->value("policy") != job->arguments.value("policyVersion")
            || ticket->value("profile") != profile.version()) {
            complete(job, error("COMMAND_TICKET_SCOPE_INVALID")); return;
        }
        const QString executionId = ticket->value("execution").toString();
        if (executions.contains(executionId)) {
            const auto previous = executions.value(executionId);
            if (previous.connection != job->connection) { complete(job, error("COMMAND_TICKET_SCOPE_INVALID")); return; }
            complete(job, previous.complete ? previous.payload : error("COMMAND_IN_PROGRESS", true, 250));
            return;
        }
        if (ticket->value("expires").toDouble() <= clock.elapsed()) { complete(job, error("COMMAND_TICKET_EXPIRED")); return; }
        if (ticket->value("grant").toString() != versionString(access.version(connection.client))) {
            complete(job, error("COMMAND_PERMISSION_REQUIRED")); return;
        }
        const QString policyVersion = CommandPolicy::version(profile);
        if (ticket->value("policy") != policyVersion) { complete(job, error("COMMAND_POLICY_CHANGED")); return; }
        if (!job->arguments.value("arguments").toObject().isEmpty()) { complete(job, error("COMMAND_NOT_ALLOWED")); return; }
        if (!facade || !facade->isAvailable() || entry.targetFingerprint.isEmpty()) {
            complete(job, error(profile.isAvailable()
                ? "COMMAND_UNAVAILABLE" : "COMMAND_PROFILE_UNAVAILABLE")); return;
        }
        if (entry.state != SessionState::Running) { complete(job, error("SESSION_NOT_READY")); return; }
        int count = 0;
        for (const auto& value : executions) if (value.connection == job->connection) ++count;
        if (guards.markers().contains(entry.targetFingerprint)) {
            complete(job, error("COMMAND_EXECUTION_QUARANTINED")); return;
        }
        if (count >= 32 || executions.size() >= 128 || lastCommand.contains(entry.targetFingerprint)) {
            complete(job, error("BUSY", true, 5000)); return;
        }
        QString rejected;
        if (!guards.reserve(entry.targetFingerprint, executionId, instance, rejected)) {
            complete(job, failure(rejected, rejected, rejected == "BUSY", 250)); return;
        }
        if (!observedFacades.contains(facade)) {
            observedFacades.insert(facade);
            QObject::connect(facade, &SessionCommandFacade::finished, q,
                [this](const CommandExecutionResult& result) { finishExecution(result); });
            QObject::connect(facade, &QObject::destroyed, q,
                [this, facade] { observedFacades.remove(facade); });
        }
        const quint64 request = nextCommandRequest++;
        Execution execution{executionId, job->connection, connection.client, entry.id, entry.epoch,
            commandId, policyVersion, entry.targetFingerprint, request,
            clock.elapsed(), facade, job, {}, false, false, {}, {}};
        executions.insert(executionId, execution);
        CommandExecutionRequest executionRequest{
            request, definition->command, {CommandOutputBytes, 5000},
            commandId, {}, 0};
        if (facade->capabilities().mode == CommandExecutionMode::InteractiveFramed
            && entry.session && entry.session->commandCoordinator()) {
            executionRequest.executionNonce = newId().toUtf8();
            executionRequest.expectedPromptGeneration =
                entry.session->commandCoordinator()->promptGeneration();
        }
        if (!facade->execute(executionRequest)) {
            executions.remove(executionId);
            guards.release(entry.targetFingerprint, executionId);
            complete(job, error("BUSY", true, 250)); return;
        }
        lastCommand.insert(entry.targetFingerprint, clock.elapsed());
        job->deadline = clock.elapsed() + 5000;
        emit q->changed();
    }

    void finishExecution(const CommandExecutionResult& result)
    {
        auto found = executions.end();
        for (auto it = executions.begin(); it != executions.end(); ++it) {
            if (it->executorRequestId == result.requestId) { found = it; break; }
        }
        if (found == executions.end()) return;
        if (found->complete) {
            // 超时响应已经发布；迟到的确定退出只解除保护，不再发第二次 RPC 响应。
            if (!result.executionMayHaveStarted || result.terminationConfirmed) {
                guards.release(found->target, found->id);
                emit q->changed();
            }
            return;
        }
        auto& execution = *found;
        execution.complete = true;
        bool truncated = result.outputTruncated;
        qsizetype remaining = CommandOutputBytes;
        const auto out = outputText(result.standardOutput, remaining, truncated);
        const auto err = outputText(result.standardError, remaining, truncated);
        const bool completed = result.outcome == CommandExecutionOutcome::Completed
            && !truncated && result.terminationConfirmed
            && (result.exitCode ? *result.exitCode == 0
                                : execution.interactive);
        QString state = completed ? "completed" : "failed";
        QString code = "COMMAND_FAILED";
        if (truncated) code = "COMMAND_OUTPUT_LIMIT";
        else if (result.outcome == CommandExecutionOutcome::TimedOut) { code = "COMMAND_TIMEOUT"; state = "timed_out"; }
        else if (result.outcome == CommandExecutionOutcome::Cancelled) { code = "COMMAND_OUTCOME_UNKNOWN"; state = "cancelled"; }
        else if (result.executionMayHaveStarted && !result.terminationConfirmed) { code = "COMMAND_OUTCOME_UNKNOWN"; state = "unknown"; }
        QJsonObject data{{"executionId", execution.id}, {"instanceId", instance}, {"sessionId", execution.session},
            {"epoch", execution.epoch}, {"commandId", execution.commandId},
            {"policyVersion", execution.policyVersion},
            {"status", state}, {"executionMayHaveStarted", result.executionMayHaveStarted},
            {"terminationConfirmed", result.terminationConfirmed}, {"startedAt", QJsonValue(QJsonValue::Null)},
            {"finishedAt", result.terminationConfirmed ? QJsonValue(utcNow()) : QJsonValue(QJsonValue::Null)},
            {"exitCode", result.exitCode ? QJsonValue(*result.exitCode) : QJsonValue(QJsonValue::Null)},
            {"stdout", out}, {"stderr", err}, {"outputTruncated", truncated}};
        execution.payload = completed ? success(data) : failure(code, code);
        if (!completed) {
            auto detail = execution.payload.value("error").toObject();
            detail.insert("details", data);
            execution.payload.insert("error", detail);
        }
        if (!result.executionMayHaveStarted || result.terminationConfirmed)
            guards.release(execution.target, execution.id);
        if (execution.job) complete(execution.job, execution.payload);
        execution.job.reset();
        emit q->changed();
    }

    Service* q;
    QString stateDirectory;
    QString instanceDirectory;
    QString instance;
    QString address;
    QString status{"Disabled"};
    AccessStore access;
    SessionDirectory sessions;
    LocalMcpServer io;
    TargetGuard guards;
    QThreadPool workers;
    QElapsedTimer clock;
    QTimer timer;
    bool started{false};
    bool listening{false};
    bool scheduled{false};
    QHash<QString, Connection> connections;
    QHash<QString, std::shared_ptr<Job>> jobs;
    QHash<QString, PendingConfirmation> confirmations;
    QHash<QString, qint64> consumedConfirmations;
    QHash<QString, QString> activeConfirmations;
    QHash<QString, qint64> confirmationExpiries;
    QHash<QString, ConfirmationScope> activeConfirmationScopes;
    QHash<quint64, ScriptWriteTask> scriptWrites;
    QHash<QString, QString> scriptPathLocks;
    QSet<ISessionScriptProvider*> observedScriptProviders;
    QSet<SessionInputArbiter*> observedInputArbiters;
    QHash<QString, Capture> captures;
    QHash<QString, Execution> executions;
    QHash<QString, qint64> lastCapture;
    QHash<QString, quint64> lastRevision;
    QHash<QString, std::shared_ptr<const TerminalContextProvider::Snapshot>> lastBase;
    QHash<QString, qint64> lastCommand;
    QSet<SessionCommandFacade*> observedFacades;
    QSet<SessionCommandCoordinator*> observedCoordinators;
    quint64 nextCommandRequest{quint64(1) << 60};
    quint64 nextScriptWrite{quint64(1) << 59};
    quint64 requestCount{0}, rejectedCount{0}, completedCount{0}, cancelledCount{0}, reusedCaptureCount{0};
    quint64 coreCaptureCount{0}, snapshotPublishCount{0}, snapshotReuseCount{0};
    quint64 coalescedReadCount{0}, projectionCopiedBytes{0};
    QHash<QString, NovaTerm::PublishedContextStatistics> publishedStats;
    qsizetype peakRequests{0};
    qint64 maximumRequestMs{0};
    std::deque<qint64> captureDurations;
    std::deque<qint64> snapshotAges;
};

Service::Service(QString stateDir, QString runtimeDir, std::unique_ptr<CredentialStore> credentials, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, std::move(stateDir), std::move(runtimeDir), std::move(credentials))) {}
Service::~Service() { stop(); }
AccessStore& Service::access() { return _impl->access; }
SessionDirectory& Service::directory() { return _impl->sessions; }
QString Service::instanceId() const { return _impl->instance; }
QString Service::endpoint() const { return _impl->address; }
QString Service::status() const { return _impl->status; }
void Service::stop() { _impl->stop(); }
QJsonObject Service::clientConfiguration(const QString& clientId) const
{
    const auto token = _impl->access.exportToken(clientId);
    if (!token) return {};
    const auto command = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
        "novaterm-mcp.exe"
#else
        "novaterm-mcp"
#endif
    );
    const bool multipleInstances = QDir(_impl->instanceDirectory).entryList({"*.json"}, QDir::Files).size() > 1;
    const QJsonArray arguments = multipleInstances ? QJsonArray{"--instance", _impl->instance} : QJsonArray{};
    return {{"mcpServers", QJsonObject{{"novaterm", QJsonObject{{"command", command},
        {"args", arguments}, {"env", QJsonObject{{"NOVATERM_MCP_TOKEN", QString::fromLatin1(*token)},
        {"NOVATERM_MCP_RUNTIME_DIR", _impl->instanceDirectory}}}}}}}};
}
QJsonObject Service::ccSwitchConfiguration(const QString& clientId) const
{
    const auto configuration = clientConfiguration(clientId);
    return configuration.value(QStringLiteral("mcpServers")).toObject()
        .value(QStringLiteral("novaterm")).toObject();
}
QJsonArray Service::executionRecords() const
{
    QJsonArray result;
    for (const auto& execution : _impl->executions)
        result.append(QJsonObject{{"executionId", execution.id}, {"commandId", execution.commandId},
            {"sessionId", execution.session}, {"complete", execution.complete}, {"result", execution.payload}});
    return result;
}
QJsonObject Service::statistics() const
{
    std::vector<qint64> durations(_impl->captureDurations.begin(), _impl->captureDurations.end());
    std::sort(durations.begin(), durations.end());
    const double p95 = durations.empty() ? 0.0 : double(durations[(durations.size() * 95 + 99) / 100 - 1]) / 1000000.0;
    std::vector<qint64> ages(_impl->snapshotAges.begin(), _impl->snapshotAges.end());
    std::sort(ages.begin(), ages.end());
    const qint64 ageP95 = ages.empty() ? 0 : ages[(ages.size() * 95 + 99) / 100 - 1];
    return {{"requests", QString::number(_impl->requestCount)}, {"rejected", QString::number(_impl->rejectedCount)},
        {"completed", QString::number(_impl->completedCount)}, {"cancelled", QString::number(_impl->cancelledCount)},
        {"reusedCaptures", QString::number(_impl->reusedCaptureCount)}, {"peakRequests", int(_impl->peakRequests)},
        {"coreCaptureCount", QString::number(_impl->coreCaptureCount)},
        {"snapshotPublishCount", QString::number(_impl->snapshotPublishCount)},
        {"snapshotReuseCount", QString::number(_impl->snapshotReuseCount)},
        {"coalescedReadCount", QString::number(_impl->coalescedReadCount)},
        {"projectionCopiedBytes", QString::number(_impl->projectionCopiedBytes)},
        {"snapshotAgeP95Ms", QString::number(ageP95)},
        {"activeClients", int(_impl->connections.size())}, {"captures", int(_impl->captures.size())},
        {"captureP95Ms", p95}, {"maximumRequestMs", _impl->maximumRequestMs}};
}
QJsonObject Service::protectedTargets() const { return _impl->guards.markers(); }
bool Service::acknowledgeTarget(const QString& fingerprint)
{
    for (const auto& execution : _impl->executions)
        if (!execution.complete && execution.target == fingerprint) return false;
    const bool ok = _impl->guards.acknowledge(fingerprint);
    emit changed();
    return ok;
}
}
