/** @file McpService.cpp
 *  @brief 有界 MCP 请求处理；所有 Session 访问均位于 GUI 线程。
 */
#include "McpService.h"
#include "TargetSessionSharing.h"
#include "FileResourceAdapter.h"
#include "FileTransfers.h"
#include "CapabilityAdmission.h"
#include "capabilities/terminal/SearchCapabilities.h"
#include "capabilities/terminal/LegacyTerminalMapping.h"
#include "capabilities/McpCapabilityAdapter.h"
#include "TerminalInteraction.h"
#include "McpProtocol.h"
#include "protocol/ToolCodec.h"
#include "HistoryCapabilityAdapter.h"
#include "ResourceComposition.h"
#include "protocol/HistoryToolCodec.h"
#include "CapabilityAdapter.h"
#include "LogReadAdapter.h"
#include "TerminalClearAdapter.h"
#include "TerminalTextWait.h"
#include "TerminalTasks.h"
#include "application/SavedTargets.h"
#include "application/CaptureStore.h"
#include "ObservationService.h"
#include "application/RequestScheduler.h"
#include "tasks/ReadUseCases.h"
#include "CommandSessionAdapter.h"
#include "protocol/BackendEvents.h"
#include "CommandPolicy.h"
#include "HttpMcpEndpoint.h"
#include "session/SessionTerminalController.h"
#include "session/SessionCommandFacade.h"
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
#include <map>
#include <mutex>

#ifndef NOVATERM_VERSION
#define NOVATERM_VERSION "0.2.30"
#endif

namespace NovaTerm::Mcp {
namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString requestKey(const QString& connection, const QString& id) { return connection + '/' + id; }
QJsonObject error(const char* code, bool retryable = false, int retryAfter = 0)
{
    return failure(QString::fromLatin1(code), QString::fromLatin1(code), retryable, retryAfter);
}
/** @brief 请求中的会话身份；list_sessions 没有会话。 */
std::optional<SessionReference> requestSession(const ToolRequest& request)
{
    return sessionReference(request);
}
}

class Service::Impl
{
public:
    struct Connection {
        QString client;
        QByteArray key;
        std::shared_ptr<Capabilities::McpCapabilityAdapter> capabilities;
    };
    struct SearchMailbox {
        explicit SearchMailbox(Service* service) : receiver(service) {}
        std::mutex mutex;
        Service* receiver;
    };
    struct PendingRequest {
        QString connection, id, tool;
        ToolRequest request;
        std::optional<HistoryToolRequest> historyRequest;
        std::optional<ClearRequest> clearRequest;
        std::optional<WaitTextRequest> waitRequest;
        std::optional<TerminalTaskRequest> taskRequest;
        std::optional<ListSavedTargetsRequest> targetsRequest;
        std::optional<ListFileResourcesRequest> fileResourcesRequest;
        std::optional<FileTransferRequest> fileTransferRequest;
        std::optional<TargetConnectionRequest> targetConnectionRequest;
        QByteArray confirmationDigest;
        QString requestState;
        std::optional<ConfirmationAnswer> answer;
        bool mrtr{false}, formElicitation{false};
        qint64 deadline{0}, created{0};
        std::optional<Capabilities::InvocationContext> invocation;
        std::optional<Capabilities::InvocationLease> lease;
        std::shared_ptr<std::atomic<bool>> cancelled{std::make_shared<std::atomic<bool>>(false)};
    };
    PendingRequest* findRequest(const QString& key) const {
        const auto found = requests.find(key);
        return found == requests.end() ? nullptr : found->second.get();
    }
    static std::optional<SessionReference> referenceOf(const PendingRequest& pending) {
        // File permissions are independent from terminal reading. The transfer
        // adapter checks its own current complete binding and four-way grants.
        if (pending.fileTransferRequest) return std::nullopt;
        if (pending.waitRequest) return SessionReference{pending.waitRequest->session, pending.waitRequest->epoch};
        if (pending.taskRequest) return std::visit([](const auto& value) {
            return SessionReference{value.session, value.epoch};
        }, *pending.taskRequest);
        if (pending.clearRequest) return SessionReference{pending.clearRequest->session, pending.clearRequest->epoch};
        return requestSession(pending.request);
    }
    std::unique_ptr<PendingRequest> takeRequest(const QString& key) {
        const auto found = requests.find(key);
        if (found == requests.end()) return {};
        auto request = std::move(found->second);
        requests.erase(found);
        return request;
    }
    QStringList requestKeys() const {
        QStringList keys;
        for (const auto& item : requests) keys.append(item.first);
        return keys;
    }

    Impl(Service* owner, QString stateDir, QString runtimeDir, std::unique_ptr<CredentialStore> credentials, quint16 port,
         std::shared_ptr<Resources::ResourceSharingService> resources)
        : q(owner), stateDirectory(stateDir.isEmpty() ? dataDirectory() : std::move(stateDir)),
          instanceDirectory(runtimeDir.isEmpty() ? runtimeDirectory() : std::move(runtimeDir)),
          access(stateDirectory, std::move(credentials), owner),
          sessions(owner), history(access, resources ? std::move(resources) : makeResourceSharingService(access, stateDirectory), owner),
          io(HttpMcpEndpoint::Backend{
              [this](const QByteArray& token) { return access.authenticate(token); },
              [this](const QString& id, const QString& client) {
                  if (connections.size() >= MaxClients) return false;
                  Connection connection;
                  connection.client = client;
                  connection.key = randomBytes();
                  connection.capabilities = std::make_shared<Capabilities::McpCapabilityAdapter>(
                      capabilityAuthorization, Capabilities::PrincipalId{client});
                  connections.insert(id, std::move(connection));
                  emit q->changed();
                  return true;
              },
              [this](const QString& id, const BackendEvent& event) { receive(id, event); },
              [this](const QString& id) { disconnect(id); }
          }, owner)
    {
        clock.start();
        history.bindDispatcher(capabilityDispatcher);
        terminal = std::make_unique<TerminalInteraction>(&access, &sessions, &observation, capabilityDispatcher, owner);
        terminal->onChanged = [this] { emit q->changed(); };
        instance = newId();
        clears = std::make_unique<TerminalClearAdapter>(access, sessions, instance, logCursorKey, owner);
        textWaits = std::make_unique<TerminalTextWait>(access, sessions, observation, instance, logCursorKey, owner);
        terminalTasks = std::make_unique<TerminalTasks>(access, sessions, *terminal, instance, logCursorKey, owner);
        QObject::connect(&savedTargets, &SavedTargets::changed, q, &Service::changed);
        targetSharing = std::make_unique<TargetSessionSharing>(access, sessions, savedTargets, owner);
        fileResources = std::make_unique<FileResourceAdapter>(access, sessions, owner);
        QObject::connect(fileResources.get(), &FileResourceAdapter::changed, q, &Service::changed);
        fileTransfers = std::make_unique<FileTransfers>(access, sessions, *fileResources, instance, owner);
        commands = std::make_unique<CommandSessionAdapter>(access, sessions,
            QDir(stateDirectory).filePath("targets"), instance, CommandSessionAdapter::Delivery{
                [this](const TaskId& task, QJsonObject payload) {
                    complete(task.str(), std::move(payload));
                },
                [this](const TaskId& task, std::chrono::milliseconds remaining) {
                    if (auto* pending = findRequest(task.str())) {
                        pending->deadline = clock.elapsed() + remaining.count();
                        if (pending->lease) pending->lease->setDeadline(Capabilities::Clock::now() + remaining);
                    }
                },
                [this](const QString& peer, const QJsonObject& message) { return io.send(peer, message); },
                [this] { emit q->changed(); }}, capabilityDispatcher);
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
                const auto pendingKeys = requestKeys();
                for (const auto& key : pendingKeys) {
                    auto* pending = findRequest(key);
                    if (!pending) continue;
                    if (pending->tool != QStringLiteral("novaterm_run_script")
                        && pending->tool != QStringLiteral("novaterm_clear_terminal")
                        && pending->tool != QStringLiteral("novaterm_run_command")
                        && pending->tool != QStringLiteral("novaterm_execute_command"))
                        continue;
                    const auto reference = referenceOf(*pending);
                    if (!reference)
                        continue;
                    const auto entry = sessions.find(reference->session.str());
                    if (!entry || entry->state != SessionState::Running)
                        cancel(key, error("SESSION_NOT_READY"), true);
                }
            }
            emit q->changed();
        });
        QObject::connect(&sessions, &SessionDirectory::invalidated, q, [this](const QString& id) {
            observation.removeSession(id);
            const auto jobsCopy = requestKeys();
            for (const auto& key : jobsCopy) {
                auto* pending = findRequest(key);
                if (!pending) continue;
                const auto reference = referenceOf(*pending);
                if (reference && reference->session.str() == id)
                    cancel(key, error("STALE_SESSION_EPOCH"), true);
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
            // 关闭入口会收口任务 owner。再次启用建立新索引，不自动恢复旧输入。
            terminalTasks = std::make_unique<TerminalTasks>(access, sessions, *terminal, instance, logCursorKey, q);
            started = true;
            status = "Starting";
            io.start(listenPort);
        }
    }
    void stop()
    {
        if (targetSharing) targetSharing->clear();
        if (fileResources) fileResources->resources().revoke();
        if (terminalTasks) terminalTasks->stop();
        if (textWaits) textWaits->revoke({}, {});
        if (terminal) terminal->revoke({}, {});
        started = listening = false;
        address.clear();
        const auto connectionIds = connections.keys();
        for (const auto& id : connectionIds)
            disconnect(id);
        commands->stop();
        fileTransfers->stop();
        io.stop();
        QFile::remove(manifestPath());
        timer.stop();
        started = listening = false;
        if (searchMailbox) {
            const std::lock_guard<std::mutex> lock(searchMailbox->mutex);
            searchMailbox->receiver = nullptr;
        }
        searchMailbox.reset();
        if (workers) {
            workers->clear();
            // GUI不等待文件/计算线程退出；worker只持有值和已关闭的投递邮箱。
            auto retiring = std::move(workers);
            QThreadPool::globalInstance()->start(QRunnable::create(
                [pool = retiring.release()] {
                    const std::unique_ptr<QThreadPool> owner(pool);
                    owner->waitForDone();
                }));
        }
    }

    void receive(const QString& connectionId, const BackendEvent& event)
    {
        auto connection = connections.find(connectionId);
        if (connection == connections.end()) { io.closeConnection(connectionId); return; }
        const auto id = std::visit([](const auto& value) { return value.id; }, event);
        if (id.isEmpty() || id.size() > 64) { io.closeConnection(connectionId); return; }
        if (const auto* result = std::get_if<BackendElicitationResult>(&event)) {
            commands->confirm(connectionId, id, result->answer);
            return;
        }
        if (std::get_if<BackendCancel>(&event)) {
            const auto pending = findRequest(requestKey(connectionId, id));
            if (pending) cancel(requestKey(connectionId, id), {}, false);
            return;
        }
        const auto* historyCall = std::get_if<BackendHistoryCall>(&event);
        const auto* clearCall = std::get_if<BackendClearCall>(&event);
        const auto* waitCall = std::get_if<BackendWaitTextCall>(&event);
        const auto* taskCall = std::get_if<BackendTaskCall>(&event);
        const auto* targetsCall = std::get_if<BackendSavedTargetsCall>(&event);
        const auto* filesCall = std::get_if<BackendFileResourcesCall>(&event);
        const auto* transferCall = std::get_if<BackendFileTransferCall>(&event);
        const auto* targetConnectionCall = std::get_if<BackendTargetConnectionCall>(&event);
        BackendCall call;
        if (transferCall) {
            call.id = transferCall->id;
            if (transferCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (filesCall) {
            call.id = filesCall->id;
            if (filesCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (targetConnectionCall) {
            call.id = targetConnectionCall->id;
            if (targetConnectionCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (targetsCall) {
            call.id = targetsCall->id;
            if (targetsCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (waitCall) {
            call.id = waitCall->id;
            if (waitCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (taskCall) {
            call.id = taskCall->id;
            if (taskCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (historyCall) {
            call.id = historyCall->id;
            if (historyCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else if (clearCall) {
            call.id = clearCall->id;
            if (clearCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else call = std::get<BackendCall>(event);
        if (findRequest(requestKey(connectionId, id))) {
            io.closeConnection(connectionId);
            return;
        }
        const auto name = transferCall ? ToolCodec::toolName(ToolCall{transferCall->request})
            : filesCall ? QStringLiteral("novaterm_list_file_resources")
            : targetConnectionCall ? ToolCodec::toolName(ToolCall{targetConnectionCall->request})
            : targetsCall ? QStringLiteral("novaterm_list_targets")
            : waitCall ? QStringLiteral("novaterm_wait_text")
            : taskCall ? ToolCodec::toolName(ToolCall{taskCall->request})
            : clearCall ? QStringLiteral("novaterm_clear_terminal")
            : historyCall ? HistoryToolCodec::toolName(historyCall->request) : ToolCodec::toolName(call.request);
        ++requestCount;
        auto request = call.request;
        if (scheduler.queuedCount(connectionId) >= Budget::MaxQueuedRequests) {
            ++rejectedCount;
            send(connectionId, id, error("BUSY", true, 250));
            return;
        }
        auto requestOwner = std::make_unique<PendingRequest>();
        auto* pending = requestOwner.get();
        pending->connection = connectionId;
        pending->id = id;
        pending->tool = name;
        if (historyCall) pending->historyRequest = historyCall->request;
        if (clearCall) pending->clearRequest = clearCall->request;
        if (waitCall) pending->waitRequest = waitCall->request;
        if (taskCall) pending->taskRequest = taskCall->request;
        if (targetsCall) pending->targetsRequest = targetsCall->request;
        if (filesCall) pending->fileResourcesRequest = filesCall->request;
        if (transferCall) pending->fileTransferRequest = transferCall->request;
        if (targetConnectionCall) pending->targetConnectionRequest = targetConnectionCall->request;
        pending->confirmationDigest = call.confirmationDigest;
        pending->mrtr = call.mrtr;
        pending->formElicitation = call.formElicitation;
        pending->requestState = call.requestState;
        pending->answer = call.answer;
        if (pending->requestState.size() > 4096 || (!pending->mrtr
            && (!pending->requestState.isEmpty() || pending->answer.has_value()))) {
            send(connectionId, id, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        pending->deadline = clock.elapsed() + (name == "novaterm_run_script" ? ScriptWriteTimeout.count()
            : name == "novaterm_run_command" ? ConfirmationTimeout.count() : 2000);
        if (clearCall) pending->deadline = clock.elapsed() + 5000;
        if (taskCall) pending->deadline = clock.elapsed() + 5000;
        if (waitCall) pending->deadline = clock.elapsed() + waitCall->request.wait.count() + Budget::WireDeadlineMargin.count();
        if (historyCall && std::holds_alternative<SearchHistoryRequest>(historyCall->request)) pending->deadline = clock.elapsed() + 12000;
        const auto waitMilliseconds = [](const ToolRequest& value, int fallback) {
            return std::visit([fallback](const auto& request) -> int {
                using Type = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Type, ReadContextRequest>
                    || std::is_same_v<Type, InteractTerminalRequest>
                    || std::is_same_v<Type, RequestElevationRequest>) {
                    return request.wait ? int(request.wait->count()) : fallback;
                } else {
                    return fallback;
                }
            }, value);
        };
        if (name == "novaterm_interact_terminal" || name == "novaterm_request_elevation"
            || (name == "novaterm_read_context" && waitMilliseconds(request, -1) >= 0))
            pending->deadline = clock.elapsed() + waitMilliseconds(request, Budget::MaxTerminalWaitMs)
            + Budget::WireDeadlineMargin.count();
        pending->request = std::move(request);
        pending->created = clock.elapsed();
        Capabilities::ResourceBinding resource{Capabilities::DirectoryBinding{}};
        if (const auto reference = referenceOf(*pending)) {
            const auto entry = sessions.find(reference->session.str());
            if (entry && entry->epoch == reference->epoch.str())
                resource = capabilityBinding(access.grantSnapshot(ClientId{connection->client}, *entry).binding);
        }
        if (historyCall) resource = history.binding(*pending->historyRequest);
        const auto version = historyCall ? history.accessVersion(connection->client, resource) : access.version(connection->client);
        pending->invocation = connection->capabilities->context(std::move(resource),
            version, Capabilities::OperationId{newId()},
            Capabilities::Clock::now() + std::chrono::milliseconds(pending->deadline - clock.elapsed()));
        requests.emplace(requestKey(connectionId, id), std::move(requestOwner));
        peakRequests = (std::max)(peakRequests, qsizetype(requests.size()));
        if (scheduler.enqueue(connectionId, id, [this, key=requestKey(connectionId, id)] { if (auto* pending = findRequest(key)) execute(pending); })
            != RequestScheduler::Admission::Accepted) {
            requests.erase(requestKey(connectionId, id));
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
                const auto pending = findRequest(requestKey(delivery.peer, delivery.request));
                if (!pending || *pending->cancelled)
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
    void finishRequest(std::unique_ptr<PendingRequest> pending, QJsonObject payload, bool reply)
    {
        const auto key = requestKey(pending->connection, pending->id);
        terminal->cancel(key);
        clears->cancel(key);
        textWaits->discard(key);
        terminalTasks->cancel(key);
        if (!*pending->cancelled) {
            if (const auto reference = referenceOf(*pending)) {
                const auto entry = sessions.find(reference->session.str());
                const auto client = connections.value(pending->connection).client;
                if (!entry || !access.canRead(client, *entry)) payload = error("SESSION_NOT_AVAILABLE");
                else if (entry->epoch != reference->epoch.str()) payload = error("STALE_SESSION_EPOCH");
            }
        }
        if (pending->lease && !pending->lease->finish() && !*pending->cancelled)
            payload = error("SESSION_NOT_AVAILABLE");
        scheduler.complete(pending->connection, pending->id);
        ++completedCount;
        maximumRequestMs = (std::max)(maximumRequestMs, clock.elapsed() - pending->created);
        if (reply && !payload.isEmpty() && connections.contains(pending->connection))
            send(pending->connection, pending->id, std::move(payload));
        if (requests.empty()) {
            if (observation.captures().size() == 0 && commands->idle()) timer.stop();
            else { timer.setInterval(1000); timer.start(); }
        }
        schedule();
    }
    void complete(const QString& key, QJsonObject payload)
    {
        if (auto owned = takeRequest(key))
            finishRequest(std::move(owned), std::move(payload), true);
    }
    void cancel(const QString& key, QJsonObject payload, bool reply)
    {
        auto owned = takeRequest(key);
        if (!owned) return;
        *owned->cancelled = true;
        if (owned->invocation)
            capabilityDispatcher.cancel(owned->invocation->principal, owned->invocation->operation);
        scheduler.cancel(owned->connection, owned->id);
        ++cancelledCount;
        commands->cancel(TaskId{key});
        finishRequest(std::move(owned), std::move(payload), reply);
    }
    void disconnect(const QString& connection)
    {
        fileTransfers->disconnect(connection);
        commands->disconnect(connection);
        const auto copy = requestKeys();
        for (const auto& key : copy) {
            auto* pending = findRequest(key);
            if (!pending) continue;
            if (pending->connection == connection)
                cancel(key, {}, false);
        }
        scheduler.removePeer(connection);
        connections.remove(connection);
        observation.captures().removeConnection(connection);
        emit q->changed();
    }
    void revoke(const QString& client, const QString& session)
    {
        if (session.isEmpty()) savedTargets.revoke(client);
        if (terminal) terminal->revoke(client, session);
        if (textWaits) textWaits->revoke(client, session);
        if (terminalTasks) terminalTasks->revoke(client, session);
        const auto ids = connections.keys();
        for (const auto& id : ids) {
            if (!client.isEmpty() && connections.value(id).client != client)
                continue;
            if (session.isEmpty()) {
                io.closeConnection(id);
                disconnect(id);
            } else {
                const auto copy = requestKeys();
                for (const auto& key : copy) {
                    auto* pending = findRequest(key);
                    if (!pending) continue;
                    const auto reference = referenceOf(*pending);
                    if (pending->connection == id && reference && reference->session.str() == session)
                        cancel(key, error("SESSION_NOT_AVAILABLE"), true);
                }
                observation.captures().removeConnectionSession(id, session);
            }
        }
        commands->revoke(client, session);
    }
    void expireJobs()
    {
        for (const auto& key : requestKeys()) {
            auto* pending = findRequest(key);
            if (!pending || pending->deadline > clock.elapsed()) continue;
            commands->expire(TaskId{key});
            if ((pending = findRequest(key))) cancel(key, error("DEADLINE_EXCEEDED"), true);
        }
        cleanup();
        if (requests.empty()) {
            if (observation.captures().size() == 0 && commands->idle()) timer.stop();
            else timer.setInterval(1000);
        }
    }
    void cleanup()
    {
        observation.captures().sweep(clock.elapsed());
        commands->sweep();
    }

    bool admit(PendingRequest* pending)
    {
        if (!pending->invocation) { complete(requestKey(pending->connection, pending->id), error("SESSION_NOT_AVAILABLE")); return false; }
        auto capability = pending->clearRequest ? Capabilities::CapabilityId{"terminal.clear"}
            : pending->waitRequest ? Capabilities::CapabilityId{"terminal.text.wait"}
            : terminalCapability(pending->request);
        if (pending->taskRequest) capability = Capabilities::CapabilityId{
            std::holds_alternative<StartTerminalTaskRequest>(*pending->taskRequest) ? "terminal.task.start"
            : std::holds_alternative<StopTerminalTaskRequest>(*pending->taskRequest) ? "terminal.task.stop" : "terminal.task.query"};
        if (pending->targetsRequest) capability = Capabilities::CapabilityId{"target.list"};
        if (pending->fileResourcesRequest) capability = Capabilities::CapabilityId{"file.resources.list"};
        if (pending->fileTransferRequest) capability = Capabilities::CapabilityId{
            std::holds_alternative<StartFileUploadRequest>(*pending->fileTransferRequest) ? "file.upload"
            : std::holds_alternative<QueryFileTransferRequest>(*pending->fileTransferRequest) ? "file.transfer.query" : "file.transfer.cancel"};
        if (pending->targetConnectionRequest) capability = Capabilities::CapabilityId{
            pending->targetConnectionRequest->action == TargetConnectionAction::Query ? "target.query"
            : pending->targetConnectionRequest->action == TargetConnectionAction::Close ? "target.close" : "target.open"};
        auto result = capabilityDispatcher.admit(capability, *pending->invocation);
        if (const auto* reason = std::get_if<Capabilities::Rejection>(&result)) {
            complete(requestKey(pending->connection, pending->id),
                *reason == Capabilities::Rejection::DeadlineExceeded ? error("DEADLINE_EXCEEDED")
                : *reason == Capabilities::Rejection::Busy ? error("BUSY", true, 250)
                : error("SESSION_NOT_AVAILABLE"));
            return false;
        }
        pending->lease = std::move(std::get<Capabilities::InvocationLease>(result));
        return true;
    }

    void execute(PendingRequest* pending)
    {
        const auto key = requestKey(pending->connection, pending->id);
        cleanup();
        pending = findRequest(key);
        if (!pending) return;
        if (pending->deadline <= clock.elapsed()) { complete(key, error("DEADLINE_EXCEEDED")); return; }
        if ((!pending->requestState.isEmpty() || pending->answer.has_value())
            && pending->tool != "novaterm_run_command"
            && pending->tool != "novaterm_run_script") {
            complete(key, error("COMMAND_CONFIRMATION_STALE"));
            return;
        }
        const auto connection = connections.value(pending->connection);
        if (!access.enabled()) { complete(key, error("MCP_DISABLED")); return; }
        if (pending->fileTransferRequest) {
            if (!admit(pending)) return;
            const auto result = fileTransfers->call(connection.client, access.identityVersion(connection.client),
                pending->connection, *pending->fileTransferRequest);
            if (const auto* failure = std::get_if<Error>(&result)) complete(key, ToolCodec::encodeFailure(*failure));
            else complete(key, success(std::get<QJsonObject>(result)));
            return;
        }
        if (pending->fileResourcesRequest) {
            if (!admit(pending)) return;
            const auto request = *pending->fileResourcesRequest;
            const auto result = fileResources->list(connection.client, access.identityVersion(connection.client), request);
            if (isError(result)) { complete(key, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
            const auto& directory = std::get<FileResourceDirectory>(result);
            const auto project = [](const FileRoot& root, bool remote) {
                QJsonObject row{{remote ? "scopeId" : "resourceId", root.id}, {"title", root.title},
                    {"revision", root.revision}, {"readAllowed", root.access.read}, {"writeAllowed", root.access.write}};
                if (remote) row.insert("path", root.path);
                return row;
            };
            QJsonArray local, remote;
            for (const auto& root : directory.local) local.append(project(root, false));
            for (const auto& scope : directory.remote) {
                auto row = project(scope.root, true);
                row.insert("sessionId", scope.binding.session.str()); row.insert("epoch", scope.binding.epoch.str());
                remote.append(row);
            }
            complete(key, success({{"instanceId", instance}, {"availability", "authorization_only"},
                {"localResources", local}, {"remoteScopes", remote}}));
            return;
        }
        if (pending->targetConnectionRequest) {
            if (!admit(pending)) return;
            const auto request = *pending->targetConnectionRequest;
            const auto validation = pending->lease->validation();
            const auto cancelled = pending->cancelled;
            const auto result = targetConnections.call(connection.client, access.identityVersion(connection.client), request,
                [validation, cancelled] { return !*cancelled && !validation().has_value(); });
            if (result.error) { complete(key, ToolCodec::encodeFailure(Error{*result.error})); return; }
            const auto& facts = result.facts;
            const auto nullable = [](const QString& value) -> QJsonValue {
                return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
            };
            complete(key, success({{"instanceId", instance}, {"targetId", request.targetId},
                {"configVersion", request.configVersion}, {"operationId", request.operationId},
                {"created", facts.created}, {"reused", !facts.created && !facts.sessionId.isEmpty()},
                {"sessionId", nullable(facts.sessionId)}, {"epoch", nullable(facts.epoch)},
                {"state", facts.state}, {"reason", facts.reason}, {"suggestedQueryMs", 500}}));
            return;
        }
        if (pending->targetsRequest) {
            if (!admit(pending)) return;
            listTargets(pending);
            return;
        }
        if (pending->historyRequest) {
            const QPointer<Service> guard(q);
            history.call(*pending->historyRequest, *pending->invocation, pending->cancelled, [this, guard, key](QJsonObject result) {
                if (guard) complete(key, std::move(result));
            });
            return;
        }
        if (!pending->clearRequest && !pending->waitRequest && !pending->taskRequest
            && std::holds_alternative<ListSessionsRequest>(pending->request)) {
            listSessions(pending);
            return;
        }
        const auto reference = referenceOf(*pending);
        if (!reference) { complete(key, error("INVALID_ARGUMENT")); return; }
        const auto entry = sessions.find(reference->session.str());
        pending = findRequest(key);
        if (!pending) return;
        if (!entry || !access.canRead(connection.client, *entry)) {
            complete(key, !pending->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("SESSION_NOT_AVAILABLE"));
            return;
        }
        if (entry->epoch != reference->epoch.str()) {
            complete(key, !pending->requestState.isEmpty()
                ? error("COMMAND_CONFIRMATION_STALE")
                : error("STALE_SESSION_EPOCH"));
            return;
        }
        if (pending->waitRequest || pending->taskRequest) {
            if (!admit(pending)) return;
            const QPointer<Service> guard(q);
            auto done = [this, guard, key](QJsonObject payload) { if (guard) complete(key, std::move(payload)); };
            if (pending->waitRequest) textWaits->call(connection.client, key, *pending->waitRequest, std::move(done));
            else {
                if (!std::holds_alternative<QueryTerminalTaskRequest>(*pending->taskRequest)
                    && commands->quarantined(entry->targetFingerprint)) {
                    complete(key, error("COMMAND_EXECUTION_QUARANTINED")); return;
                }
                terminalTasks->call(connection.client, key, *pending->taskRequest, std::move(done), *pending->invocation);
            }
            return;
        }
        if (pending->clearRequest) {
            if (!admit(pending)) return;
            const QPointer<Service> guard(q);
            clears->call(connection.client, key, *pending->clearRequest, [this, guard, key](QJsonObject payload) {
                if (guard) complete(key, std::move(payload));
            });
            return;
        }
        const bool mutatingInteraction = std::holds_alternative<InteractTerminalRequest>(pending->request)
            || std::holds_alternative<RequestElevationRequest>(pending->request);
        bool usesObservation = mutatingInteraction;
        if (const auto* read = std::get_if<ReadContextRequest>(&pending->request)) {
            usesObservation = read->source != ContextSource::Log && (read->projection.has_value() || read->wait.has_value()
                || read->waitFor.has_value() || read->expandToken.has_value()
                || terminal->ownsCursor(connection.client,
                    read->sinceToken.value_or(QString{})));
        } else if (const auto* search = std::get_if<SearchContextRequest>(&pending->request)) {
            usesObservation = terminal->ownsCapture(connection.client, search->capture.str());
        }
        if (usesObservation) {
            if (!admit(pending)) return;
            if (mutatingInteraction && commands->quarantined(entry->targetFingerprint)) {
                complete(key, error("COMMAND_EXECUTION_QUARANTINED")); return;
            }
            const QPointer<Service> guard(q);
            const auto request = pending->request;
            terminal->call(connection.client, key, request,
                [this, guard, key](QJsonObject value) {
                    if (guard)
                        complete(key, std::move(value));
                }, connection.key, *pending->invocation);
            return;
        }
        if (std::holds_alternative<ListCommandsRequest>(pending->request)) { listCommands(pending, *entry); return; }
        if (std::holds_alternative<ExecuteCommandRequest>(pending->request)
            || std::holds_alternative<RunCommandRequest>(pending->request)
            || std::holds_alternative<RunScriptRequest>(pending->request)) {
            commands->start(CommandCall{TaskId{requestKey(pending->connection, pending->id)},
                pending->connection, pending->id, ClientId{connection.client}, connection.key,
                pending->request, pending->confirmationDigest,
                pending->requestState, pending->answer, pending->mrtr, pending->formElicitation}, *entry, *pending->invocation);
            return;
        }
        if (entry->state == SessionState::Closing || entry->state == SessionState::Closed) {
            complete(key, error("SESSION_CLOSED")); return;
        }
        if (entry->state != SessionState::Running && entry->state != SessionState::Failed) {
            complete(key, error("SESSION_NOT_READY")); return;
        }
        if (std::holds_alternative<ReadContextRequest>(pending->request)) readContext(pending, *entry);
        else search(pending, *entry);
    }

    TargetConnectionFacts queryTargetConnection(const QString& id, const QString& attachment)
    {
        const auto entry = sessions.find(id);
        if (!entry || !entry->session) return {id, {}, attachment, "closed", "session_unavailable", false};
        if (entry->state == SessionState::Closed)
            return {id, entry->epoch, attachment, "closed", {}, false};
        if (!entry->transport || entry->transport->property("novatermSavedConnectionId").toString() != attachment)
            return {id, {}, attachment, "invalidated", "attachment_changed", false};
        const auto reason = entry->transport->property("novatermHostKeyDecisionPending").toBool()
            ? QStringLiteral("host_key_confirmation_required") : QString{};
        return {entry->id, entry->epoch, attachment, SessionDirectory::stateName(entry->state), reason, false};
    }

    TargetConnectionResult closeTargetConnection(const TargetConnectionFacts& original,
        const QString& epoch, const TargetConnections::Guard& allowed)
    {
        QPointer<TerminalSession> session;
        const auto safety = [this, &original, &epoch, &session]() -> std::optional<ErrorCode> {
            const auto entry = sessions.find(original.sessionId);
            if (!entry || !entry->session || !entry->transport
                || entry->transport->property("novatermSavedConnectionId").toString() != original.attachmentId)
                return ErrorCode::TargetCloseNotAllowed;
            session = entry->session;
            if (entry->epoch != epoch) return ErrorCode::StaleSessionEpoch;
            if (entry->generation != original.connectionGeneration || session->manualConnectionGeneration()
                || session->inputArbiter()->userInputGeneration() || session->core()->localInputGeneration())
                return ErrorCode::TargetCloseNotAllowed;
            auto* controller = session->terminalController();
            if (entry->state != SessionState::Running || !entry->transport->isConnected()
                || entry->transport->hasPendingDisconnect()
                || entry->transport->property("novatermHostKeyDecisionPending").toBool()
                || !session->commandCoordinator()->hasTrustedProfile()
                || controller->pending() || controller->commandActive() || !controller->readyForCommand()
                || session->inputArbiter()->hasMcpLease() || session->inputArbiter()->hasTransferLease()
                || session->commandFacade()->hasActiveRequests() || commands->busy(entry->id) || fileTransfers->busy(entry->id)
                || commands->quarantined(entry->targetFingerprint)) return ErrorCode::TargetCloseBusy;
            return {};
        };
        if (!allowed()) return {{}, ErrorCode::TargetOpenPermissionRequired};
        if (const auto error = safety()) return {{}, error};
        // The local UI owns SFTP panels, including inactive tabs. Unknown SSH facts refuse close.
        const auto localSafety = targetCloseSafety;
        if ((localSafety && !localSafety(original.sessionId))
            || (!localSafety && session->runtimeConfig().transportKind == TransportKind::Ssh))
            return {{}, ErrorCode::TargetCloseBusy};
        if (!allowed()) return {{}, ErrorCode::TargetOpenPermissionRequired};
        if (const auto error = safety()) return {{}, error};
        if (!allowed() || !session) return {{}, ErrorCode::TargetOpenPermissionRequired};
        session->close(CloseMode::Graceful);
        auto facts = queryTargetConnection(original.sessionId, original.attachmentId);
        facts.created = true;
        return {facts, {}};
    }

    void listTargets(PendingRequest* pending)
    {
        const auto key = requestKey(pending->connection, pending->id);
        const auto connection = connections.value(pending->connection);
        const auto request = *pending->targetsRequest;
        const auto version = access.identityVersion(connection.client);
        QString after, revision;
        if (!request.cursor.isEmpty()) {
            const auto cursor = verifyToken(request.cursor, logCursorKey);
            if (!cursor || cursor->value("kind") != "saved_targets"
                || cursor->value("client") != connection.client
                || cursor->value("identityVersion") != QString::number(version)
                || !cursor->value("revision").isString() || cursor->value("revision").toString().isEmpty()
                || !cursor->value("after").isString() || cursor->value("after").toString().isEmpty()) {
                complete(key, error("CURSOR_MISMATCH")); return;
            }
            after = cursor->value("after").toString(); revision = cursor->value("revision").toString();
        }
        const auto page = savedTargets.page(connection.client, version, request.limit, after, revision);
        if (!page.error.isEmpty()) { complete(key, failure(page.error, page.error)); return; }
        QJsonArray rows;
        for (const auto& target : page.targets)
            rows.append(QJsonObject{{"targetId", target.id}, {"title", target.title},
                {"transport", target.transport}, {"endpoint", target.endpoint},
                {"configVersion", target.configVersion}, {"availability", "saved_configuration"}});
        QJsonValue next(QJsonValue::Null);
        if (!page.nextAfter.isEmpty()) next = signToken({{"kind", "saved_targets"},
            {"client", connection.client}, {"identityVersion", QString::number(version)},
            {"revision", page.revision}, {"after", page.nextAfter}}, logCursorKey);
        complete(key, success({{"instanceId", instance}, {"targets", rows}, {"nextCursor", next}}));
    }

    void listSessions(PendingRequest* pending)
    {
        const auto key = requestKey(pending->connection, pending->id);
        const auto connection = connections.value(pending->connection);
        const auto request = std::get<ListSessionsRequest>(pending->request);
        const ReadUseCaseAdapter reads{{&access, &sessions, &observation, instance},
            capabilityDispatcher, *pending->invocation};
        const auto result = reads.listSessions(connection.client, connection.key,
            pending->formElicitation, request);
        if (isError(result)) { complete(key, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
        auto data = std::get<QJsonObject>(result);
        auto rows = data.value("sessions").toArray();
        for (qsizetype i = 0; i < rows.size(); ++i) {
            auto row = rows[i].toObject(); auto availability = row.value("availability").toObject();
            availability.insert("terminalTasks", terminalTasks->capabilities(connection.client, row.value("sessionId").toString()));
            availability.insert("textWait", true);
            row.insert("availability", availability); rows[i] = row;
        }
        data.insert("sessions", rows);
        complete(key, success(data));
    }

    void readContext(PendingRequest* pending, const SessionDirectory::Entry& entry)
    {
        const auto key = requestKey(pending->connection, pending->id);
        const auto connection = connections.value(pending->connection);
        const auto request = std::get<ReadContextRequest>(pending->request);
        if (request.source == ContextSource::Log) {
            if (!admit(pending)) return;
            const auto result = readLogContext(access, sessions, instance, connection.client, logCursorKey, request);
            if (isError(result)) { complete(key, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
            complete(key, success(std::get<QJsonObject>(result)));
            return;
        }
        const ReadUseCaseAdapter reads{{&access, &sessions, &observation, instance},
            capabilityDispatcher, *pending->invocation};
        const auto result = reads.readContext(entry, pending->connection, connection.client, connection.key, request);
        if (isError(result)) { complete(key, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
        complete(key, success(std::get<QJsonObject>(result)));
    }

    void search(PendingRequest* pending, const SessionDirectory::Entry& entry)
    {
        const auto key = requestKey(pending->connection, pending->id);
        const auto typed = std::get<SearchContextRequest>(pending->request);
        const auto captured = observation.captures().find(typed.capture.str());
        if (!captured || captured->scope != CaptureScope::Connection || captured->owner != pending->connection
            || captured->session != entry.id || captured->epoch != entry.epoch
            || !entry.session || captured->cacheGeneration != entry.session->core()->cacheGeneration()) {
            complete(key, error("CAPTURE_NOT_AVAILABLE")); return;
        }
        if (!workers) {
            workers = std::make_unique<QThreadPool>();
            workers->setMaxThreadCount(1); workers->setExpiryTimeout(1000);
            searchMailbox = std::make_shared<SearchMailbox>(q);
        }
        const auto mailbox = searchMailbox;
        const auto cancelled = pending->cancelled;
        const Capabilities::Terminal::SearchCapabilities ports{capabilityDispatcher,
            [record = *captured](const auto&, const auto&) -> Result<CaptureRecord> { return record; },
            [this, mailbox, cancelled](CaptureRecord record, SearchContextRequest request, auto done) {
                workers->start(QRunnable::create([mailbox, cancelled, record = std::move(record), request = std::move(request), done = std::move(done)] {
                    NovaTerm::setCurrentThreadName("nvterm-mcp-srch");
                    QElapsedTimer timer; timer.start();
                    const auto scope = request.scope == SearchScope::Viewport ? QStringLiteral("viewport")
                        : request.scope == SearchScope::RecentOutput ? QStringLiteral("recent_output") : QStringLiteral("both");
                    const auto found = searchCapture(record, request.query, request.caseSensitive, scope, request.maxMatches,
                        [cancelled, &timer] { return *cancelled || timer.elapsed() > 100; });
                    if (*cancelled) return;
                    const std::lock_guard<std::mutex> lock(mailbox->mutex);
                    if (!mailbox->receiver) return;
                    QMetaObject::invokeMethod(mailbox->receiver, [cancelled, found, done] {
                        if (!*cancelled) done(found);
                    }, Qt::QueuedConnection);
                }));
            }};
        const QPointer<Service> guard(q);
        ports.search.invoke(typed, *pending->invocation,
            [this, guard, key, session = entry.id, epoch = entry.epoch, generation = captured->cacheGeneration](auto value) {
            if (!guard) return;
            const auto current = sessions.find(session);
            if (!current || !current->session || current->epoch != epoch
                || current->session->core()->cacheGeneration() != generation) {
                complete(key, error("CAPTURE_NOT_AVAILABLE")); return;
            }
            const auto* domain = std::get_if<Result<Capabilities::Terminal::ContextSearchResult>>(&value);
            if (!domain) { complete(key, error("SESSION_NOT_AVAILABLE")); return; }
            if (const auto* failure = errorIfAny(*domain)) { complete(key, ToolCodec::encodeFailure(*failure)); return; }
            const auto& result = std::get<Capabilities::Terminal::ContextSearchResult>(*domain);
            QJsonArray matches;
            for (const auto& match : result.search.matches) matches.append(QJsonObject{{"source", match.source},
                {"lineIndex", match.lineIndex}, {"startByte", match.startByte}, {"endByte", match.endByte}});
            complete(key, success({{"captureId", result.capture.str()}, {"revision", result.revision}, {"matches", matches},
                {"limited", result.search.limited}, {"sourceTruncated", result.sourceTruncated}, {"outputTruncated", result.outputTruncated}}));
        });
    }

    void listCommands(PendingRequest* pending, const SessionDirectory::Entry& entry)
    {
        const auto key = requestKey(pending->connection, pending->id);
        const auto connection = connections.value(pending->connection);
        const ReadUseCaseAdapter reads{{&access, &sessions, &observation, instance},
            capabilityDispatcher, *pending->invocation};
        const auto result = reads.listCommands(entry, connection.client, connection.key,
            clock.elapsed());
        if (isError(result)) { complete(key, ToolCodec::encodeFailure(*errorIfAny(result))); return; }
        complete(key, success(std::get<QJsonObject>(result)));
    }

    Service* q;
    QString stateDirectory;
    QString instanceDirectory;
    QString instance;
    QString address;
    QString status{"Disabled"};
    AccessStore access;
    SessionDirectory sessions;
    SavedTargets savedTargets;
    TargetConnections targetConnections{savedTargets};
    std::unique_ptr<TargetSessionSharing> targetSharing;
    std::unique_ptr<FileResourceAdapter> fileResources;
    std::unique_ptr<FileTransfers> fileTransfers;
    std::function<bool(const QString&)> targetCloseSafety;
    HistoryCapabilityAdapter history;
    Capabilities::CapabilityRegistry capabilityRegistry{Capabilities::CapabilityRegistry::implementedCapabilities()};
    Capabilities::CapabilityAuthorization capabilityAuthorization{
        [this](const Capabilities::PrincipalId& principal, Capabilities::EntryKind kind,
               const Capabilities::ResourceBinding& binding, const Capabilities::Definition& definition)
            -> std::optional<Capabilities::AuthorizationSnapshot> {
            if (kind != Capabilities::EntryKind::Mcp || !access.enabled() || !access.version(principal.value()))
                return std::nullopt;
            if (definition.policy == Capabilities::Policy::HistoryRead) return history.snapshot(principal.value(), binding);
            if (const auto* target = std::get_if<Capabilities::SessionBinding>(&binding)) {
                const auto entry = sessions.find(target->session.value());
                if (!entry || !access.canRead(principal.value(), *entry)) return std::nullopt;
                if (definition.policy == Capabilities::Policy::ClearTerminal
                    && !access.canClearTerminal(principal.value(), *entry)) return std::nullopt;
                const auto current = capabilityBinding(access.grantSnapshot(ClientId{principal.value()}, *entry).binding);
                return Capabilities::AuthorizationSnapshot{current, access.version(principal.value())};
            }
            if (!std::holds_alternative<Capabilities::DirectoryBinding>(binding)) return std::nullopt;
            return Capabilities::AuthorizationSnapshot{binding, access.version(principal.value())};
        }};
    Capabilities::CapabilityDispatcher capabilityDispatcher{capabilityRegistry, capabilityAuthorization};
    HttpMcpEndpoint io;
    quint16 listenPort{0};
    QElapsedTimer clock;
    ObservationService observation{[this] { return qint64(clock.elapsed()); }};
    const QByteArray logCursorKey{randomBytes()};
    std::unique_ptr<TerminalInteraction> terminal;
    std::unique_ptr<TerminalClearAdapter> clears;
    std::unique_ptr<TerminalTextWait> textWaits;
    std::unique_ptr<TerminalTasks> terminalTasks;
    std::unique_ptr<QThreadPool> workers;
    std::shared_ptr<SearchMailbox> searchMailbox;
    QTimer timer;
    bool started{false};
    bool listening{false};
    bool scheduled{false};
    bool checkingSessionState{false};
    QHash<QString, Connection> connections;
    RequestScheduler scheduler;
    std::map<QString, std::unique_ptr<PendingRequest>> requests;
    std::unique_ptr<CommandSessionAdapter> commands;
    quint64 requestCount{0}, rejectedCount{0}, completedCount{0}, cancelledCount{0};
    qsizetype peakRequests{0};
    qint64 maximumRequestMs{0};
};

Service::Service(QString stateDir, QString runtimeDir, std::unique_ptr<CredentialStore> credentials, QObject* parent, quint16 port,
                 std::shared_ptr<Resources::ResourceSharingService> resources)
    : QObject(parent), _impl(std::make_unique<Impl>(this, std::move(stateDir), std::move(runtimeDir), std::move(credentials), port, std::move(resources))) {}
Service::~Service() { stop(); }
AccessStore& Service::access() { return _impl->access; }
SessionDirectory& Service::directory() { return _impl->sessions; }
SavedTargets& Service::savedTargets() { return _impl->savedTargets; }
FileResourceAdapter& Service::fileResources() { return *_impl->fileResources; }
void Service::setFileUploadStarter(FileTransfers::Start start) { _impl->fileTransfers->setStarter(std::move(start)); }
void Service::setTargetOpener(TargetConnections::Open open)
{
    auto* impl = _impl.get();
    TargetConnections::Open factory;
    if (open) factory = [impl, open = std::move(open)](const SavedTargetSource& source, const TargetConnections::Guard& allowed) {
        auto facts = open(source, allowed);
        if (facts.sessionId.isEmpty()) return facts;
        const auto entry = impl->sessions.find(facts.sessionId);
        if (!entry || !entry->session || !entry->transport)
            return TargetConnectionFacts{facts.sessionId, {}, {}, "unavailable", "session_unavailable", facts.created};
        // Transport object identity survives initial host-key discovery and same-target reconnects.
        auto id = entry->transport->property("novatermSavedConnectionId").toString();
        if (id.isEmpty()) {
            id = newId(); entry->transport->setProperty("novatermSavedConnectionId", id);
        }
        auto current = impl->queryTargetConnection(entry->id, id);
        current.created = facts.created;
        current.connectionGeneration = entry->generation;
        return current;
    };
    _impl->targetConnections.setPorts(std::move(factory), [impl](const QString& id, const QString& attachment) {
        return impl->queryTargetConnection(id, attachment);
    }, [impl](const TargetConnectionFacts& facts, const QString& epoch, const TargetConnections::Guard& allowed) {
        return impl->closeTargetConnection(facts, epoch, allowed);
    }, [impl](const QString& client, quint64 identity, const TargetConnectionRequest& request,
        const TargetConnectionFacts& facts, const NewSessionSharing& sharing) {
        impl->targetSharing->created(client, identity, request, facts, sharing);
    });
}
void Service::setTargetCloseSafety(std::function<bool(const QString&)> safety)
{
    _impl->targetCloseSafety = std::move(safety);
}
HistoryCapabilityAdapter& Service::history() { return _impl->history; }
std::shared_ptr<Resources::ResourceSharingService> Service::resources() const { return _impl->history.resources(); }
void Service::setHistorySource(Recording::HistoryService* source) { _impl->history.setSource(source); }
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
QJsonArray Service::executionRecords() const { return _impl->commands->executionRecords(); }
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
QJsonObject Service::protectedTargets() const { return _impl->commands->protectedTargets(); }
bool Service::acknowledgeTarget(const QString& fingerprint)
{ return _impl->commands->acknowledgeTarget(fingerprint); }
bool Service::acknowledgeTarget(const QString& fingerprint, const QString& executionId)
{ return !executionId.isEmpty() && _impl->commands->acknowledgeTarget(fingerprint, executionId); }
}
