/** @file McpService.cpp
 *  @brief 有界 MCP 请求处理；所有 Session 访问均位于 GUI 线程。
 */
#include "McpService.h"
#include "McpProtocol.h"
#include "CommandPolicy.h"
#include "LocalMcpServer.h"
#include "session/SessionCommandFacade.h"
#include "core/ThreadNaming.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <deque>

#ifndef NOVATERM_VERSION
#define NOVATERM_VERSION "0.2.17"
#endif

namespace NovaTerm::Mcp {
namespace {
QString utcNow() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString jobKey(const QString& connection, const QString& id) { return connection + '/' + id; }
QString versionString(quint64 version) { return QString::number(version); }
QJsonObject error(const char* code, bool retryable = false, int retryAfter = 0)
{
    return failure(QString::fromLatin1(code), QString::fromLatin1(code), retryable, retryAfter);
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
        qint64 deadline{0};
        qint64 created{0};
        bool running{false};
        std::atomic<bool> cancelled{false};
    };
    struct Connection {
        QString client;
        QByteArray key;
        int active{0};
        std::deque<std::shared_ptr<Job>> waiting;
    };
    struct Capture {
        QString connection;
        QString session;
        QString epoch;
        QJsonObject data;
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
        QString target;
        quint64 executorRequestId{0};
        qint64 submitted{0};
        QPointer<SessionCommandFacade> facade;
        std::shared_ptr<Job> job;
        QJsonObject payload;
        bool complete{false};
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
                if (execution.facade)
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
            connections.insert(connectionId, Connection{client, randomBytes(), 0, {}});
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
        job->deadline = clock.elapsed() + 2000;
        job->created = clock.elapsed();
        jobs.insert(jobKey(connectionId, id), job);
        peakRequests = std::max(peakRequests, jobs.size());
        connection->waiting.push_back(job);
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
    void complete(const std::shared_ptr<Job>& job, QJsonObject payload)
    {
        if (!jobs.contains(jobKey(job->connection, job->id)))
            return;
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
        if (jobs.isEmpty()) timer.stop();
        schedule();
    }
    void cancel(const std::shared_ptr<Job>& job, QJsonObject payload, bool reply)
    {
        if (job->cancelled.exchange(true))
            return;
        ++cancelledCount;
        for (auto& execution : executions) {
            if (!execution.complete && execution.job == job && execution.facade)
                execution.facade->cancel(execution.executorRequestId);
        }
        if (reply && connections.contains(job->connection))
            send(job->connection, job->id, payload);
        complete(job, {});
    }
    void disconnect(const QString& connection)
    {
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
                    if (execution.facade)
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
    }
    void cleanup()
    {
        const auto now = clock.elapsed();
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
        const auto connection = connections.value(job->connection);
        if (!access.enabled()) { complete(job, error("MCP_DISABLED")); return; }
        if (job->tool == "novaterm_list_sessions") { listSessions(job); return; }
        const auto entry = sessions.find(job->arguments.value("sessionId").toString());
        if (job->cancelled) return;
        if (!entry || !access.canRead(connection.client, *entry)) { complete(job, error("SESSION_NOT_AVAILABLE")); return; }
        if (entry->epoch != job->arguments.value("epoch").toString()) { complete(job, error("STALE_SESSION_EPOCH")); return; }
        if (job->tool == "novaterm_list_commands") { listCommands(job, *entry); return; }
        if (job->tool == "novaterm_execute_command") { command(job, *entry); return; }
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
            if (!access.commands(connection.client, entry).isEmpty()) capabilities.append("execute_command");
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
        std::shared_ptr<const TerminalContextProvider::Snapshot> base;
        if (lastCapture.contains(entry.id) && now - lastCapture.value(entry.id) < 250) {
            const auto current = entry.session->core()->tryModelRevision();
            if (!current || lastRevision.value(entry.id) != *current) {
                complete(job, error("BUSY", true, 250)); return;
            }
            ++reusedCaptureCount;
            base = lastBase.value(entry.id);
        }
        if (!base) {
            base = entry.session->tryTerminalContext();
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
        QJsonObject data{{"instanceId", instance}, {"sessionId", entry.id}, {"epoch", entry.epoch},
            {"revision", versionString(base->state.revision)}, {"captureId", captureId}, {"capturedAt", utcNow()},
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
        captures.insert(captureId, Capture{job->connection, entry.id, entry.epoch, data, now + CaptureLifetimeMs, now});
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
        const auto data = capture->data;
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
            QMetaObject::invokeMethod(guard, [this, guard, job, result] { if (guard) complete(job, result); }, Qt::QueuedConnection);
        }));
    }

    void listCommands(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        const auto allowed = access.commands(connection.client, entry);
        QString reason;
        auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
        if (entry.state != SessionState::Running) reason = "SESSION_NOT_READY";
        else if (!facade || !facade->isAvailable()) reason = "UNSUPPORTED_COMMAND_TARGET";
        else if (allowed.isEmpty()) reason = "COMMAND_PERMISSION_REQUIRED";
        else if (entry.targetFingerprint.isEmpty()) reason = "COMMAND_UNAVAILABLE";
        QJsonArray catalog;
        if (reason.isEmpty()) for (const auto& command : CommandPolicy::catalog()) {
            if (!allowed.contains(command.id)) continue;
            const auto ticket = signToken({{"kind", "command"}, {"execution", newId()},
                {"session", entry.id}, {"epoch", entry.epoch}, {"command", command.id},
                {"policy", CommandPolicy::version()}, {"grant", versionString(access.version(connection.client))},
                {"expires", clock.elapsed() + 60000}}, connection.key);
            catalog.append(QJsonObject{{"commandId", command.id}, {"title", command.title},
                {"preview", QString::fromUtf8(command.command)},
                {"argumentSchema", QJsonObject{{"type", "object"}, {"additionalProperties", false}}},
                {"timeoutMs", 5000}, {"maxOutputBytes", CommandOutputBytes}, {"commandTicket", ticket}});
        }
        complete(job, success({{"instanceId", instance}, {"sessionId", entry.id}, {"epoch", entry.epoch},
            {"policyVersion", CommandPolicy::version()}, {"executionEnabled", reason.isEmpty()},
            {"disabledReason", reason.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(reason)}, {"commands", catalog}}));
    }

    void command(const std::shared_ptr<Job>& job, const SessionDirectory::Entry& entry)
    {
        const auto connection = connections.value(job->connection);
        const auto commandId = job->arguments.value("commandId").toString();
        const auto definition = CommandPolicy::find(commandId);
        if (!definition) { complete(job, error("COMMAND_NOT_ALLOWED")); return; }
        if (!access.commands(connection.client, entry).contains(commandId)) {
            complete(job, error("COMMAND_PERMISSION_REQUIRED")); return;
        }
        const auto ticket = verifyToken(job->arguments.value("commandTicket").toString(), connection.key);
        if (!ticket || ticket->value("kind") != "command" || ticket->value("session") != entry.id
            || ticket->value("epoch") != entry.epoch || ticket->value("command") != commandId
            || ticket->value("policy") != job->arguments.value("policyVersion")) {
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
        if (ticket->value("policy") != CommandPolicy::version()) { complete(job, error("COMMAND_POLICY_CHANGED")); return; }
        if (!job->arguments.value("arguments").toObject().isEmpty()) { complete(job, error("COMMAND_NOT_ALLOWED")); return; }
        auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
        if (!facade || !facade->isAvailable() || entry.targetFingerprint.isEmpty()) {
            complete(job, error("UNSUPPORTED_COMMAND_TARGET")); return;
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
            commandId, entry.targetFingerprint, request, clock.elapsed(), facade, job, {}, false};
        executions.insert(executionId, execution);
        if (!facade->execute(CommandExecutionRequest{
                request, definition->command, {CommandOutputBytes, 5000}})) {
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
        const bool completed = result.outcome == CommandExecutionOutcome::Completed && !truncated
            && result.exitCode.has_value() && *result.exitCode == 0;
        QString state = completed ? "completed" : "failed";
        QString code = "COMMAND_FAILED";
        if (truncated) code = "COMMAND_OUTPUT_LIMIT";
        else if (result.outcome == CommandExecutionOutcome::TimedOut) { code = "COMMAND_TIMEOUT"; state = "timed_out"; }
        else if (result.outcome == CommandExecutionOutcome::Cancelled) { code = "COMMAND_OUTCOME_UNKNOWN"; state = "cancelled"; }
        else if (result.executionMayHaveStarted && !result.terminationConfirmed) { code = "COMMAND_OUTCOME_UNKNOWN"; state = "unknown"; }
        QJsonObject data{{"executionId", execution.id}, {"instanceId", instance}, {"sessionId", execution.session},
            {"epoch", execution.epoch}, {"commandId", execution.commandId}, {"policyVersion", CommandPolicy::version()},
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
    QHash<QString, Capture> captures;
    QHash<QString, Execution> executions;
    QHash<QString, qint64> lastCapture;
    QHash<QString, quint64> lastRevision;
    QHash<QString, std::shared_ptr<const TerminalContextProvider::Snapshot>> lastBase;
    QHash<QString, qint64> lastCommand;
    QSet<SessionCommandFacade*> observedFacades;
    quint64 nextCommandRequest{quint64(1) << 60};
    quint64 requestCount{0}, rejectedCount{0}, completedCount{0}, cancelledCount{0}, reusedCaptureCount{0};
    qsizetype peakRequests{0};
    qint64 maximumRequestMs{0};
    std::deque<qint64> captureDurations;
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
    return {{"requests", QString::number(_impl->requestCount)}, {"rejected", QString::number(_impl->rejectedCount)},
        {"completed", QString::number(_impl->completedCount)}, {"cancelled", QString::number(_impl->cancelledCount)},
        {"reusedCaptures", QString::number(_impl->reusedCaptureCount)}, {"peakRequests", int(_impl->peakRequests)},
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
