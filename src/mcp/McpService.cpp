/** @file McpService.cpp
 *  @brief 有界 MCP 请求处理；所有 Session 访问均位于 GUI 线程。
 */
#include "McpService.h"
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
#include "application/CaptureStore.h"
#include "ObservationService.h"
#include "application/RequestScheduler.h"
#include "tasks/ReadUseCases.h"
#include "CommandSessionAdapter.h"
#include "protocol/BackendEvents.h"
#include "CommandPolicy.h"
#include "HttpMcpEndpoint.h"
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
                        && pending->tool != QStringLiteral("novaterm_run_command")
                        && pending->tool != QStringLiteral("novaterm_execute_command"))
                        continue;
                    const auto reference = requestSession(pending->request);
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
                const auto reference = requestSession(pending->request);
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
        commands->stop();
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
        BackendCall call;
        if (historyCall) {
            call.id = historyCall->id;
            if (historyCall->confirmationSupplied) { send(connectionId, id, error("COMMAND_CONFIRMATION_STALE")); return; }
        } else call = std::get<BackendCall>(event);
        if (findRequest(requestKey(connectionId, id))) {
            io.closeConnection(connectionId);
            return;
        }
        const auto name = historyCall ? HistoryToolCodec::toolName(historyCall->request) : ToolCodec::toolName(call.request);
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
        if (const auto reference = sessionReference(pending->request)) {
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
        if (!*pending->cancelled) {
            if (const auto reference = requestSession(pending->request)) {
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
        if (terminal) terminal->revoke(client, session);
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
                    const auto reference = requestSession(pending->request);
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
        auto result = capabilityDispatcher.admit(terminalCapability(pending->request), *pending->invocation);
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
        if (pending->historyRequest) {
            const QPointer<Service> guard(q);
            history.call(*pending->historyRequest, *pending->invocation, pending->cancelled, [this, guard, key](QJsonObject result) {
                if (guard) complete(key, std::move(result));
            });
            return;
        }
        if (std::holds_alternative<ListSessionsRequest>(pending->request)) {
            listSessions(pending);
            return;
        }
        const auto reference = requestSession(pending->request);
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
        const bool mutatingInteraction = std::holds_alternative<InteractTerminalRequest>(pending->request)
            || std::holds_alternative<RequestElevationRequest>(pending->request);
        bool usesObservation = mutatingInteraction;
        if (const auto* read = std::get_if<ReadContextRequest>(&pending->request)) {
            usesObservation = read->projection.has_value() || read->wait.has_value()
                || read->waitFor.has_value() || read->expandToken.has_value()
                || terminal->ownsCursor(connection.client,
                    read->sinceToken.value_or(QString{}));
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
        complete(key, success(std::get<QJsonObject>(result)));
    }

    void readContext(PendingRequest* pending, const SessionDirectory::Entry& entry)
    {
        const auto key = requestKey(pending->connection, pending->id);
        const auto connection = connections.value(pending->connection);
        const auto request = std::get<ReadContextRequest>(pending->request);
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
            || captured->session != entry.id || captured->epoch != entry.epoch) {
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
        ports.search.invoke(typed, *pending->invocation, [this, guard, key](auto value) {
            if (!guard) return;
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
    std::unique_ptr<TerminalInteraction> terminal;
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
