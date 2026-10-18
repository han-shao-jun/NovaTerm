/** @file McpService.cpp
 *  @brief 有界 MCP 请求处理；所有 Session 访问均位于 GUI 线程。
 */
#include "McpService.h"
#include "TerminalInteraction.h"
#include "McpProtocol.h"
#include "protocol/ToolCodec.h"
#include "CapabilityAdapter.h"
#include "application/CaptureStore.h"
#include "ObservationService.h"
#include "tasks/ConfirmationRegistry.h"
#include "application/RequestScheduler.h"
#include "application/CommandDedup.h"
#include "tasks/ExecutionRegistry.h"
#include "tasks/TargetReservation.h"
#include "tasks/Job.h"
#include "tasks/ScriptTaskRegistry.h"
#include "tasks/TaskRegistry.h"
#include "tasks/ReadUseCases.h"
#include "tasks/DiagnosticCommandUseCase.h"
#include "tasks/ScriptResultUseCase.h"
#include "tasks/ScriptCommandUseCase.h"
#include "tasks/ExecutionService.h"
#include "protocol/BackendEvents.h"
#include "CommandPolicy.h"
#include "CommandRiskPolicy.h"
#include "HttpMcpEndpoint.h"
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
#include <QScopedValueRollback>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <type_traits>
#include <utility>

#ifndef NOVATERM_VERSION
#define NOVATERM_VERSION "0.2.30"
#endif

namespace NovaTerm::Mcp {
namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString jobKey(const QString& connection, const QString& id) { return connection + '/' + id; }
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
/** @brief 请求中的会话身份；list_sessions 没有会话。 */
std::optional<SessionReference> requestSession(const ToolRequest& request)
{
    return std::visit([](const auto& value) -> std::optional<SessionReference> {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ListSessionsRequest>)
            return std::nullopt;
        else
            return SessionReference{value.session, value.epoch};
    }, request);
}
}

class Service::Impl
{
public:
    struct Connection {
        QString client;
        QByteArray key;
    };
    using ScriptWriteTask = ScriptTaskRegistry::Task;

    Impl(Service* owner, QString stateDir, QString runtimeDir, std::unique_ptr<CredentialStore> credentials, quint16 port)
        : q(owner), stateDirectory(stateDir.isEmpty() ? dataDirectory() : std::move(stateDir)),
          instanceDirectory(runtimeDir.isEmpty() ? runtimeDirectory() : std::move(runtimeDir)),
          access(stateDirectory, std::move(credentials), owner),
          sessions(owner), io(HttpMcpEndpoint::Backend{
              [this](const QByteArray& token) { return access.authenticate(token); },
              [this](const QString& id, const QString& client) {
                  if (connections.size() >= MaxClients) return false;
                  Connection connection;
                  connection.client = client;
                  connection.key = randomBytes();
                  connections.insert(id, std::move(connection));
                  emit q->changed();
                  return true;
              },
              [this](const QString& id, const BackendEvent& event) { receive(id, event); },
              [this](const QString& id) { disconnect(id); }
          }, owner), guards(QDir(stateDirectory).filePath("targets"))
    {
        clock.start();
        terminal = std::make_unique<TerminalInteraction>(&access, &sessions, &observation, owner);
        terminal->onChanged = [this] { emit q->changed(); };
        workers.setMaxThreadCount(1);
        workers.setExpiryTimeout(1000);
        instance = newId();
        scriptCommands = std::make_unique<ScriptCommandUseCase>(ScriptCommandUseCase::Ports{
            &access, &executions, &scriptWrites, &jobs, &guards, &lastCommand,
            &confirmations, &observedScriptProviders, &observedInputArbiters,
            &observedFacades, &observedCoordinators, &nextCommandRequest,
            &nextScriptWrite, instance, [this] { return clock.elapsed(); }, q,
            [this](const QString& id) { return connections.value(id).client; },
            [this](const QString& id) { return connections.value(id).key; },
            [this](const QString& id, const QJsonObject& message) {
                return io.send(id, message);
            },
            [this](const std::shared_ptr<Job>& job, QJsonObject payload) {
                complete(job, std::move(payload));
            },
            [this](const std::shared_ptr<Job>& job, bool release) {
                releaseScriptReservation(job, release);
            },
            [this](const ScriptTaskRegistry::Task& task, const ScriptWriteResult& result) {
                recordScriptWriteOutcome(task, result);
            },
            [this](const CommandExecutionResult& result) { finishExecution(result); },
            [this](TerminalSession* session, quint64 userGeneration) {
                cancelScriptWritesForUserInput(session, userGeneration);
            },
            [this](const ScriptWriteResult& result) { finishScriptWrite(result); },
            [this] { emit q->changed(); }});
        listenPort = port;
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, q, [this] { expireJobs(); });
        QObject::connect(&io, &HttpMcpEndpoint::listening, q, [this](bool ok, const QString& url) {
            if (!started) return;
            listening = ok;
            address = ok ? url : QString{};
            if (ok) {
                ok = writePrivateJson(manifestPath(), {{"instanceId", instance},
                    {"pid", QString::number(QCoreApplication::applicationPid())},
                    {"startedAt", utcNow()}, {"applicationVersion", NOVATERM_VERSION},
                    {"transport", "streamable-http"}, {"endpoint", address}});
            }
            status = ok ? QStringLiteral("Listening") : QStringLiteral("HTTP startup failed");
            if (!ok) { listening = false; address.clear(); io.stop(); }
            emit q->changed();
        });
        QObject::connect(&access, &AccessStore::changed, q, [this] { updateEnabled(); emit q->changed(); });
        QObject::connect(&access, &AccessStore::revoked, q,
            [this](const QString& client, const QString& session) { revoke(client, session); });
        QObject::connect(&sessions, &SessionDirectory::changed, q, [this] {
            if (!checkingSessionState) {
                QScopedValueRollback<bool> checking(checkingSessionState, true);
                const auto pending = jobs.snapshot();
                for (const auto& job : pending) {
                    if (job->tool != QStringLiteral("novaterm_run_script")
                        && job->tool != QStringLiteral("novaterm_run_command")
                        && job->tool != QStringLiteral("novaterm_execute_command"))
                        continue;
                    const auto reference = requestSession(job->request);
                    if (!reference)
                        continue;
                    const auto entry = sessions.find(reference->session.str());
                    if (!entry || entry->state != SessionState::Running)
                        cancel(job, error("SESSION_NOT_READY"), true);
                }
            }
            emit q->changed();
        });
        QObject::connect(&sessions, &SessionDirectory::invalidated, q, [this](const QString& id) {
            observation.removeSession(id);
            const auto jobsCopy = jobs.snapshot();
            for (const auto& job : jobsCopy) {
                const auto reference = requestSession(job->request);
                if (reference && reference->session.str() == id)
                    cancel(job, error("STALE_SESSION_EPOCH"), true);
            }
        });
        updateEnabled();
    }

    QString manifestPath() const { return QDir(instanceDirectory).filePath(instance + ".json"); }
    void updateEnabled()
    {
        if (!access.enabled()) { stop(); status = "Disabled"; return; }
        if (!started) {
            if (!secureDirectory(instanceDirectory)) { status = "Cannot secure HTTP endpoint directory"; return; }
            started = true;
            status = "Starting";
            io.start(listenPort);
        }
    }
    void stop()
    {
        if (terminal) terminal->revoke({}, {});
        started = listening = false;
        address.clear();
        const auto connectionIds = connections.keys();
        for (const auto& id : connectionIds)
            disconnect(id);
        for (const auto& execution : executions.records()) {
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

    void receive(const QString& connectionId, const BackendEvent& event)
    {
        auto connection = connections.find(connectionId);
        if (connection == connections.end()) { io.closeConnection(connectionId); return; }
        const auto id = std::visit([](const auto& value) { return value.id; }, event);
        if (id.isEmpty() || id.size() > 64) { io.closeConnection(connectionId); return; }
        if (const auto* result = std::get_if<BackendElicitationResult>(&event)) {
            const auto key = jobKey(connectionId, id);
            const auto pending = confirmations.takePending(key);
            if (!pending) return;
            const auto job = jobs.find(pending->jobKey);
            if (!job) return;
            const QString action = result->action;
            if (action != "accept" || !result->confirmed) {
                complete(job, error(action == "cancel"
                    ? "COMMAND_CONFIRMATION_CANCELLED" : "COMMAND_CONFIRMATION_DECLINED"));
                return;
            }
            const auto entry = sessions.find(pending->session);
            const auto currentConnection = connections.constFind(connectionId);
            auto* coordinator = entry && entry->session
                ? entry->session->commandCoordinator() : nullptr;
            auto* inputArbiter = entry && entry->session
                ? entry->session->inputArbiter() : nullptr;
            auto* facade = entry && entry->session
                ? entry->session->commandFacade() : nullptr;
            const bool unverifiedSsh = entry && entry->kind == TransportKind::Ssh
                && coordinator && coordinator->allowsUnverifiedPrompt();
            const bool idleSerial = entry && entry->kind == TransportKind::Serial
                && coordinator && coordinator->allowsIdleCompletion();
            const bool promptFree = unverifiedSsh || idleSerial;
            const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
            RiskAssessment currentRisk;
            QByteArray digest;
            if (pending->tool == QStringLiteral("novaterm_run_script")) {
                const auto& script = std::get<RunScriptRequest>(job->request);
                currentRisk = CommandRiskPolicy{}.classifyScript(
                    script.scriptContent.toUtf8(), script.targetPath,
                    script.workingDirectory, script.invocation);
                digest = job->confirmationDigest;
            } else {
                currentRisk = CommandRiskPolicy{}.classify(pending->command);
                digest = QCryptographicHash::hash(pending->command.toUtf8(),
                    QCryptographicHash::Sha256);
            }
            if (!entry || currentConnection == connections.cend()
                || currentConnection->client != pending->client
                || !access.canRead(pending->client, *entry)
                || (pending->tool == QStringLiteral("novaterm_run_script")
                    && !access.canRunScriptTask(pending->client, *entry))
                || (pending->tool == QStringLiteral("novaterm_run_command")
                    && !access.canRunCommand(pending->client, *entry))
                || entry->epoch != pending->epoch || !coordinator
                || (!promptFree && !coordinator->isPromptReady())
                || (promptFree && (!inputArbiter
                    || inputArbiter->userInputGeneration()
                        != pending->userInputGeneration))
                || !facade || profile.version() != pending->profileVersion
                || entry->targetFingerprint != pending->targetFingerprint
                || access.version(pending->client) != pending->grantVersion
                || coordinator->promptGeneration() != pending->promptGeneration
                || digest != pending->payloadHash
                || currentRisk.policyVersion != pending->policyVersion
                || (currentRisk.decision != RiskDecision::Confirm
                    && currentRisk.decision != RiskDecision::Unknown)
                || (pending->tool == QStringLiteral("novaterm_run_script")
                    && (!entry->session->scriptProvider()
                        || !entry->session->scriptProvider()->isAvailable()))) {
                complete(job, error("COMMAND_CONFIRMATION_STALE"));
                return;
            }
            job->confirmationAccepted = true;
            job->deadline = clock.elapsed() + Budget::ConfirmationTimeout.count();
            if (pending->tool == QStringLiteral("novaterm_run_script"))
                runScript(job, *entry);
            else
                runCommand(job, *entry);
            return;
        }
        if (std::get_if<BackendCancel>(&event)) {
            const auto job = jobs.find(jobKey(connectionId, id));
            if (job) cancel(job, {}, false);
            return;
        }
        const auto& call = std::get<BackendCall>(event);
        if (jobs.contains(jobKey(connectionId, id))) {
            io.closeConnection(connectionId);
            return;
        }
        const auto name = call.tool;
        ++requestCount;
        const auto arguments = call.arguments;
        auto decoded = ToolCodec::decode(name, arguments);
        if (isError(decoded)) {
            ++rejectedCount;
            send(connectionId, id, ToolCodec::encodeFailure(*errorIfAny(decoded)));
            return;
        }
        auto& request = std::get<ToolRequest>(decoded);
        if (scheduler.queuedCount(connectionId) >= Budget::MaxQueuedRequests) {
            ++rejectedCount;
            send(connectionId, id, error("BUSY", true, 250));
            return;
        }
        auto job = std::make_shared<Job>();
        job->connection = connectionId;
        job->id = id;
        job->tool = name;
        if (std::holds_alternative<RunScriptRequest>(request)) {
            job->confirmationDigest = scriptArgumentsDigest(arguments);
            job->scriptMetadataHash = scriptMetadataDigest(arguments);
        }
        job->mrtr = call.mrtr;
        job->formElicitation = call.formElicitation;
        job->requestState = call.requestState;
        job->inputResponses = call.inputResponses;
        if (job->requestState.size() > 4096 || (!job->mrtr
            && (!job->requestState.isEmpty() || !job->inputResponses.isEmpty()))) {
            send(connectionId, id, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        job->deadline = clock.elapsed() + (name == "novaterm_run_script" ? ScriptWriteTimeout.count()
            : name == "novaterm_run_command" ? ConfirmationTimeout.count() : 2000);
        const auto waitMilliseconds = [](const ToolRequest& value, int fallback) {
            return std::visit([fallback](const auto& request) -> int {
                using Type = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Type, ReadContextRequest>
                    || std::is_same_v<Type, InteractTerminalRequest>
                    || std::is_same_v<Type, RequestElevationRequest>) {
                    return request.wait ? int(request.wait->count()) : fallback;
                }
                return fallback;
            }, value);
        };
        if (name == "novaterm_interact_terminal" || name == "novaterm_request_elevation"
            || (name == "novaterm_read_context" && waitMilliseconds(request, -1) >= 0))
            job->deadline = clock.elapsed() + waitMilliseconds(request, Budget::MaxTerminalWaitMs)
            + Budget::WireDeadlineMargin.count();
        job->request = std::move(request);
        job->created = clock.elapsed();
        jobs.insert(jobKey(connectionId, id), job);
        peakRequests = (std::max)(peakRequests, qsizetype(jobs.size()));
        if (scheduler.enqueue(connectionId, id, [this, job] { execute(job); })
            != RequestScheduler::Admission::Accepted) {
            jobs.remove(jobKey(connectionId, id));
            ++rejectedCount;
            send(connectionId, id, error("BUSY", true, 250));
            return;
        }
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
            for (auto& delivery : scheduler.takeReady()) {
                const auto job = jobs.find(jobKey(delivery.peer, delivery.request));
                if (!job || job->cancelled)
                    continue;
                delivery.run();
            }
            if (scheduler.hasReady())
                schedule();
        });
    }

    void send(const QString& connection, const QString& id, QJsonObject payload)
    {
        QJsonObject packet{{"op", "result"}, {"id", id}, {"payload", payload}};
        // wire 统一编码最终 RPC 并核验上限；内部 DTO 不再重复序列化。
        if (!io.send(connection, packet)) {
            io.closeConnection(connection);
            disconnect(connection);
        }
    }
    bool hasPendingScriptWrite(const std::shared_ptr<Job>& job) const
    {
        return scriptWrites.containsJob(job);
    }
    void releaseScriptReservation(const std::shared_ptr<Job>& job, bool releaseTarget)
    {
        if (releaseTarget && job->reservation)
            job->reservation->release();
        job->reservation.reset();
        scriptWrites.unlockOwner(jobKey(job->connection, job->id));
    }
    ScriptResultUseCase scriptResults()
    {
        return ScriptResultUseCase{ScriptResultUseCase::Ports{
            &access, &executions, &scriptWrites, &jobs, &sessions, instance,
            [this] { return clock.elapsed(); },
            [this] { emit q->changed(); },
            [this](const std::shared_ptr<Job>& job, QJsonObject payload) {
                complete(job, std::move(payload));
            },
            [this](const std::shared_ptr<Job>& job, bool release) {
                releaseScriptReservation(job, release);
            },
            [this](const std::shared_ptr<Job>& job,
                const SessionDirectory::Entry& entry) { runCommand(job, entry); }}};
    }
    void recordScriptWriteOutcome(const ScriptWriteTask& task,
                                  const ScriptWriteResult& result)
    {
        scriptResults().recordOutcome(task, result);
    }

    void complete(const std::shared_ptr<Job>& job, QJsonObject payload)
    {
        if (terminal) terminal->cancel(jobKey(job->connection, job->id));
        if (!jobs.contains(jobKey(job->connection, job->id)))
            return;
        const QString publishedExecutionId = job->scriptPublished && job->reservation
            ? job->reservation->executionId() : QString();
        confirmations.removePending(jobKey(job->connection, job->id));
        if (!hasPendingScriptWrite(job))
            releaseScriptReservation(job, true);
        auto connection = connections.find(job->connection);
        if (connection != connections.end()) {
            const auto client = connection->client;
            if (!job->cancelled) {
                const auto reference = requestSession(job->request);
                if (reference) {
                    const auto entry = sessions.find(reference->session.str());
                    if (!jobs.contains(jobKey(job->connection, job->id)))
                        return;
                    if (!entry || !access.canRead(client, *entry))
                        payload = error("SESSION_NOT_AVAILABLE");
                    else if (entry->epoch != reference->epoch.str())
                        payload = error("STALE_SESSION_EPOCH");
                }
                // find() 可能同步发布会话失效，失效处理已经完成响应时不得再发送。
            }
        }
        if (job->scriptPublished && !payload.value("ok").toBool()) {
            auto error = payload.value("error").toObject();
            auto details = error.value("details").toObject();
            details.insert("published", true);
            details.insert("publicationMayHaveOccurred", true);
            details.insert("invocationStarted", false);
            if (!details.contains("status"))
                details.insert("status", "script_published");
            if (!publishedExecutionId.isEmpty())
                details.insert("executionId", publishedExecutionId);
            error.insert("details", details);
            payload.insert("error", error);
        }
        scheduler.complete(job->connection, job->id);
        jobs.remove(jobKey(job->connection, job->id));
        ++completedCount;
        maximumRequestMs = (std::max)(maximumRequestMs, clock.elapsed() - job->created);
        if (!job->cancelled && connections.contains(job->connection))
            send(job->connection, job->id, payload);
        if (jobs.empty()) {
            if (observation.captures().size() == 0 && executions.size() == 0
                && lastCommand.empty() && confirmations.empty()) {
                timer.stop();
            } else {
                timer.setInterval(1000);
                timer.start();
            }
        }
        schedule();
    }
    void cancel(std::shared_ptr<Job> job, QJsonObject payload, bool reply)
    {
        if (terminal) terminal->cancel(jobKey(job->connection, job->id));
        if (job->cancelled.exchange(true))
            return;
        scheduler.cancel(job->connection, job->id);
        ++cancelledCount;
        QList<std::pair<QPointer<ISessionScriptProvider>, quint64>> scriptCancels;
        for (const quint64 id : scriptWrites.ids()) {
            const auto* task = scriptWrites.find(id);
            if (task && task->job == job)
                scriptCancels.append({task->provider, id});
        }
        for (const auto& scriptCancel : scriptCancels) {
            if (scriptCancel.first)
                scriptCancel.first->cancelWrite(scriptCancel.second);
        }
        QList<quint64> unresolvedRequests;
        const auto jobId = jobKey(job->connection, job->id);
        for (const auto& execution : executions.records()) {
            if (execution.complete || execution.jobKey != jobId)
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
            QTimer::singleShot(Budget::CancelCompletionGrace, q, [this, requestId] {
                const auto* execution = executions.findByRequest(requestId);
                if (!execution || execution->complete)
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
        static_cast<void>(confirmations.takeConnection(connection));
        const auto copy = jobs.snapshot();
        for (const auto& job : copy) {
            if (job->connection == connection)
                cancel(job, {}, false);
        }
        scheduler.removePeer(connection);
        connections.remove(connection);
        observation.captures().removeConnection(connection);
        emit q->changed();
    }
    void revoke(const QString& client, const QString& session)
    {
        if (terminal) terminal->revoke(client, session);
        confirmations.removeScope(client, session);
        const auto ids = connections.keys();
        for (const auto& id : ids) {
            if (!client.isEmpty() && connections.value(id).client != client)
                continue;
            if (session.isEmpty()) {
                io.closeConnection(id);
                disconnect(id);
            } else {
                const auto copy = jobs.snapshot();
                for (const auto& job : copy) {
                    const auto reference = requestSession(job->request);
                    if (job->connection == id && reference && reference->session.str() == session)
                        cancel(job, error("SESSION_NOT_AVAILABLE"), true);
                }
                observation.captures().removeConnectionSession(id, session);
            }
        }
    }
    void expireJobs()
    {
        const auto copy = jobs.snapshot();
        const auto executionRecords = executions.records();
        for (const auto& job : copy) {
            if (job->deadline > clock.elapsed()) continue;
            bool commandExpired = false;
            const auto jobId = jobKey(job->connection, job->id);
            for (const auto& execution : executionRecords) {
                if (!execution.complete && execution.jobKey == jobId) {
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
        if (jobs.empty()) {
            if (observation.captures().size() == 0 && executions.size() == 0
                && lastCommand.empty() && confirmations.empty()) timer.stop();
            else timer.setInterval(1000);
        }
    }
    void cleanup()
    {
        const auto now = clock.elapsed();
        for (const auto& expired : confirmations.sweep(now)) {
            const auto job = jobs.find(expired.jobKey);
            if (job)
                complete(job, error("COMMAND_CONFIRMATION_EXPIRED"));
        }
        observation.captures().sweep(now);
        executions.expireCompleted(now, Budget::ExecutionRecordRetention.count());
        lastCommand.sweep(now);
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
        const auto reference = requestSession(job->request);
        if (!reference) { complete(job, error("INVALID_ARGUMENT")); return; }
        const auto entry = sessions.find(reference->session.str());
        if (job->cancelled) return;
        if (!entry || !access.canRead(connection.client, *entry)) {
            complete(job, !job->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("SESSION_NOT_AVAILABLE"));
            return;
        }
        if (entry->epoch != reference->epoch.str()) {
            complete(job, !job->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("STALE_SESSION_EPOCH"));
            return;
        }
        const bool mutatingInteraction = std::holds_alternative<InteractTerminalRequest>(job->request)
            || std::holds_alternative<RequestElevationRequest>(job->request);
        bool usesObservation = mutatingInteraction;
        if (const auto* read = std::get_if<ReadContextRequest>(&job->request)) {
            usesObservation = read->projection.has_value() || read->wait.has_value()
                || read->waitFor.has_value() || read->expandToken.has_value()
                || terminal->ownsCursor(connection.client,
                    read->sinceToken.value_or(QString{}));
        } else if (const auto* search = std::get_if<SearchContextRequest>(&job->request)) {
            usesObservation = terminal->ownsCapture(connection.client, search->capture.str());
        }
        if (usesObservation) {
            if (mutatingInteraction && guards.markers().contains(entry->targetFingerprint)) {
                complete(job, error("COMMAND_EXECUTION_QUARANTINED")); return;
            }
            const QPointer<Service> guard(q);
            terminal->call(connection.client, jobKey(job->connection, job->id), job->request,
                [this, guard, job](QJsonObject value) {
                    if (guard && !job->cancelled) complete(job, std::move(value));
                }, connection.key);
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
        const auto& request = std::get<ListSessionsRequest>(job->request);
        const ReadUseCases reads{{&access, &sessions, &observation, terminal.get(), instance}};
        const auto result = reads.listSessions(connection.client, connection.key,
            job->formElicitation, request);
        if (isError(result)) { complete(job, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
        complete(job, success(std::get<QJsonObject>(result)));
    }

    void readContext(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        const auto& request = std::get<ReadContextRequest>(job->request);
        const ReadUseCases reads{{&access, &sessions, &observation, terminal.get(), instance}};
        const auto result = reads.readContext(entry, job->connection, connection.client, connection.key, request);
        if (isError(result)) { complete(job, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
        complete(job, success(std::get<QJsonObject>(result)));
    }

    void search(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto& typed = std::get<SearchContextRequest>(job->request);
        const auto id = typed.capture.str();
        const auto captured = observation.captures().find(id);
        if (!captured || captured->scope != CaptureScope::Connection
            || captured->owner != job->connection
            || captured->session != entry.id || captured->epoch != entry.epoch) {
            complete(job, error("CAPTURE_NOT_AVAILABLE")); return;
        }
        const auto record = *captured;
        const QString query = typed.query;
        const bool caseSensitive = typed.caseSensitive;
        const QString scope = typed.scope == SearchScope::Viewport ? QStringLiteral("viewport")
            : typed.scope == SearchScope::RecentOutput ? QStringLiteral("recent_output")
            : QStringLiteral("both");
        const int maxMatches = typed.maxMatches;
        QPointer<Service> guard(q);
        workers.start(QRunnable::create([this, guard, job, record, id, query, caseSensitive, scope, maxMatches] {
            NovaTerm::setCurrentThreadName("nvterm-mcp-srch");
            QElapsedTimer timer;
            timer.start();
            // 搜索语义由 ObservationService 统一实现；这里只注入取消/超时条件。
            const auto found = observation.search(record, query,
                caseSensitive, scope, maxMatches,
                [&job, &timer] { return job->cancelled || timer.elapsed() > 100; });
            if (job->cancelled || !guard) return;
            QJsonArray matches;
            for (const auto& match : found.matches) {
                matches.append(QJsonObject{{"source", match.source},
                    {"lineIndex", match.lineIndex}, {"startByte", match.startByte},
                    {"endByte", match.endByte}});
            }
            const auto result = found.interrupted ? error("DEADLINE_EXCEEDED")
                : success({{"captureId", id}, {"revision", record.revision},
                    {"matches", matches}, {"limited", found.limited},
                    {"sourceTruncated", record.sourceTruncated},
                    {"outputTruncated", record.outputTruncated}});
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
        const ReadUseCases reads{{&access, &sessions, &observation, terminal.get(), instance}};
        const auto result = reads.listCommands(entry, connection.client, connection.key,
            clock.elapsed());
        if (isError(result)) { complete(job, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
        complete(job, success(std::get<QJsonObject>(result)));
    }

    void runScript(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        if (scriptCommands)
            scriptCommands->runScript(job, entry);
    }

    void cancelScriptWritesForUserInput(TerminalSession* session,
                                        quint64 userGeneration)
    {
        if (!session)
            return;
        const QString sessionId = session->id().toString(QUuid::WithoutBraces);
        const auto ids = scriptWrites.ids();
        for (const quint64 requestId : ids) {
            const auto* task = scriptWrites.find(requestId);
            if (task && task->session == sessionId
                && task->userInputGeneration != userGeneration) {
                cancel(task->job, error("SCRIPT_CANCELLED_BY_USER"), true);
            }
        }
    }

    void finishScriptWrite(const ScriptWriteResult& result)
    {
        scriptResults().handleResult(result);
    }

    void runCommand(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        if (scriptCommands)
            scriptCommands->runCommand(job, entry);
    }

    void command(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        DiagnosticCommandUseCase use{DiagnosticCommandUseCase::Ports{
            &access, &executions, &scriptWrites, &guards, &lastCommand,
            &observedFacades, &nextCommandRequest, instance,
            [this] { return clock.elapsed(); }, q,
            [this](const std::shared_ptr<Job>& target, QJsonObject payload) {
                complete(target, std::move(payload));
            },
            [this] { emit q->changed(); },
            [this](const CommandExecutionResult& result) { finishExecution(result); }}};
        use.run(job, entry, connection.client, connection.key);
    }

    void finishExecution(const CommandExecutionResult& result)
    {
        ExecutionService service{{&executions, &guards, &jobs, instance,
            [this](const std::shared_ptr<Job>& job, QJsonObject payload) {
                complete(job, std::move(payload));
            },
            [this] { emit q->changed(); }}};
        service.finish(result);
    }

    Service* q;
    QString stateDirectory;
    QString instanceDirectory;
    QString instance;
    QString address;
    QString status{"Disabled"};
    AccessStore access;
    SessionDirectory sessions;
    HttpMcpEndpoint io;
    quint16 listenPort{0};
    TargetGuard guards;
    QElapsedTimer clock;
    ObservationService observation{[this] { return qint64(clock.elapsed()); }};
    std::unique_ptr<TerminalInteraction> terminal;
    QThreadPool workers;
    QTimer timer;
    bool started{false};
    bool listening{false};
    bool scheduled{false};
    bool checkingSessionState{false};
    QHash<QString, Connection> connections;
    RequestScheduler scheduler;
    TaskRegistry jobs;
    ConfirmationRegistry confirmations;
    ScriptTaskRegistry scriptWrites;
    QSet<ISessionScriptProvider*> observedScriptProviders;
    QSet<SessionInputArbiter*> observedInputArbiters;
    ExecutionRegistry executions;
    CommandDedup lastCommand;
    QSet<SessionCommandFacade*> observedFacades;
    QSet<SessionCommandCoordinator*> observedCoordinators;
    std::unique_ptr<ScriptCommandUseCase> scriptCommands;
    quint64 nextCommandRequest{quint64(1) << 60};
    quint64 nextScriptWrite{quint64(1) << 59};
    quint64 requestCount{0}, rejectedCount{0}, completedCount{0}, cancelledCount{0};
    qsizetype peakRequests{0};
    qint64 maximumRequestMs{0};
};

Service::Service(QString stateDir, QString runtimeDir, std::unique_ptr<CredentialStore> credentials, QObject* parent, quint16 port)
    : QObject(parent), _impl(std::make_unique<Impl>(this, std::move(stateDir), std::move(runtimeDir), std::move(credentials), port)) {}
Service::~Service() { stop(); }
AccessStore& Service::access() { return _impl->access; }
SessionDirectory& Service::directory() { return _impl->sessions; }
QString Service::instanceId() const { return _impl->instance; }
QString Service::endpoint() const { return _impl->address; }
QString Service::httpEndpoint() const { return _impl->listening ? _impl->address : QString{}; }
QString Service::status() const { return _impl->status; }
void Service::stop() { _impl->stop(); }
QJsonObject Service::clientConfiguration(const QString& clientId) const
{
    const auto token = _impl->access.exportToken(clientId);
    if (!token) return {};
    if (httpEndpoint().isEmpty()) return {};
    return {{"mcpServers", QJsonObject{{"novaterm", QJsonObject{
        {"type", "http"}, {"url", httpEndpoint()},
        {"headers", QJsonObject{{"Authorization", "Bearer " + QString::fromLatin1(*token)}}}
    }}}}};
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
    for (const auto& execution : _impl->executions.records())
        result.append(QJsonObject{{"executionId", execution.id}, {"commandId", execution.commandId},
            {"sessionId", execution.session}, {"complete", execution.complete}, {"result", execution.payload}});
    return result;
}
QJsonObject Service::statistics() const
{
    const auto& captureDurations = _impl->observation.captureDurations();
    std::vector<qint64> durations(captureDurations.begin(), captureDurations.end());
    std::sort(durations.begin(), durations.end());
    const double p95 = durations.empty() ? 0.0 : double(durations[(durations.size() * 95 + 99) / 100 - 1]) / 1000000.0;
    const auto& snapshotAges = _impl->observation.snapshotAges();
    std::vector<qint64> ages(snapshotAges.begin(), snapshotAges.end());
    std::sort(ages.begin(), ages.end());
    const qint64 ageP95 = ages.empty() ? 0 : ages[(ages.size() * 95 + 99) / 100 - 1];
    return {{"requests", QString::number(_impl->requestCount)}, {"rejected", QString::number(_impl->rejectedCount)},
        {"completed", QString::number(_impl->completedCount)}, {"cancelled", QString::number(_impl->cancelledCount)},
        {"reusedCaptures", QString::number(_impl->observation.reusedCaptureCount())},
        {"peakRequests", int(_impl->peakRequests)},
        {"coreCaptureCount", QString::number(_impl->observation.coreCaptureCount())},
        {"snapshotPublishCount", QString::number(_impl->observation.snapshotPublishCount())},
        {"snapshotReuseCount", QString::number(_impl->observation.snapshotReuseCount())},
        {"coalescedReadCount", QString::number(_impl->observation.coalescedReadCount())},
        {"projectionCopiedBytes", QString::number(_impl->observation.projectionCopiedBytes())},
        {"snapshotAgeP95Ms", QString::number(ageP95)},
        {"activeClients", int(_impl->connections.size())}, {"captures", int(_impl->observation.captures().size())},
        {"http", _impl->io.statistics()},
        {"captureP95Ms", p95}, {"maximumRequestMs", _impl->maximumRequestMs}};
}
QJsonObject Service::protectedTargets() const { return _impl->guards.markers(); }
bool Service::acknowledgeTarget(const QString& fingerprint)
{
    for (const auto& task : _impl->scriptWrites.tasks())
        if (task.targetFingerprint == fingerprint) return false;
    for (const auto& execution : _impl->executions.records())
        if (!execution.complete && execution.target == fingerprint) return false;
    const bool ok = _impl->guards.acknowledge(fingerprint);
    emit changed();
    return ok;
}
bool Service::acknowledgeTarget(const QString& fingerprint, const QString& expectedExecutionId)
{
    if (expectedExecutionId.isEmpty())
        return false;
    for (const auto& task : _impl->scriptWrites.tasks())
        if (task.targetFingerprint == fingerprint) return false;
    for (const auto& execution : _impl->executions.records())
        if (!execution.complete && execution.target == fingerprint) return false;
    const bool ok = _impl->guards.release(fingerprint, expectedExecutionId);
    if (ok)
        emit changed();
    return ok;
}
}
