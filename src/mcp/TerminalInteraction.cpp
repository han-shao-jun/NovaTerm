#include "TerminalInteraction.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "session/SessionTerminalController.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>

namespace NovaTerm::Mcp {
namespace {
QString number(quint64 value) { return QString::number(value); }
QByteArray digest(const QJsonObject& value) {
    return QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}
bool commandAction(const QJsonObject& args) {
    const auto actions = args.value("actions").toArray();
    return !actions.isEmpty() && actions.first().toObject().value("type") == "command";
}
}

class TerminalInteraction::Impl {
public:
    struct Replay {
        quint64 generation{0}, next{0}; QString operation, epoch;
        QByteArray hash; quint64 grant{0};
    };
    struct Capture {
        QString client, session, epoch; quint64 grant{0}; qint64 expires{0};
        QJsonArray lines; // Snapshot projection only, never an unbounded terminal log.
        QString revision;
        bool sourceTruncated{false};
    };
    struct Pending {
        QString client, request, tool, session, epoch, mode, wake;
        QJsonObject args; Completion done;
        quint64 since{0}, event{0}, user{0}, control{0}, grant{0};
        qint64 deadline{0}, created{0}; bool initial{false}, interaction{false};
    };
    Impl(TerminalInteraction* owner, AccessStore* store, SessionDirectory* directory)
        : q(owner), access(store), sessions(directory), key(randomBytes()) {
        clock.start(); poll.setInterval(250);
        QObject::connect(&poll, &QTimer::timeout, q, [this] { flush(); });
        QObject::connect(sessions, &SessionDirectory::invalidated, q,
            [this](const QString& id) { revoke({}, id); });
    }
    TerminalInteraction* q;
    AccessStore* access;
    SessionDirectory* sessions;
    QByteArray key;
    QElapsedTimer clock;
    QTimer poll;
    QList<std::shared_ptr<Pending>> waiting;
    QHash<QString, Replay> replays;
    QHash<QString, Capture> captures;
    QHash<QString, quint64> lastModel;
    QHash<QString, qint64> lastOutput;
    QHash<QString, QSet<QByteArray>> deliveredHints;
    QSet<TerminalSession*> watched;
    bool flushing{false};

    void watch(TerminalSession* session) {
        if (watched.contains(session)) return;
        watched.insert(session);
        QObject::connect(session, &TerminalSession::activityChanged, q, [this] {
            if (!waiting.isEmpty()) QTimer::singleShot(0, q, [this] { flush(); });
        });
        QObject::connect(session, &TerminalSession::terminalControlChanged, q, [this] {
            if (q->onChanged) q->onChanged();
        });
        QObject::connect(session, &QObject::destroyed, q, [this, session] { watched.remove(session); });
    }
    QJsonObject capabilities(const QString& client, const QString& id) {
        const auto entry = sessions->find(id);
        if (!entry || !entry->session) return {};
        auto* controller = entry->session->terminalController();
        const bool allowed = access->canControlTerminal(client, *entry);
        const bool online = entry->state == SessionState::Running && entry->transport
            && entry->transport->isConnected() && entry->kind != TransportKind::Custom;
        const bool transfer = entry->session->inputArbiter()->hasTransferLease();
        const bool other = !controller->owner().isEmpty() && controller->owner() != client;
        const bool legacyBusy = entry->session->inputArbiter()->hasMcpLease() && controller->owner().isEmpty();
        const QString reason = !allowed ? "TERMINAL_PERMISSION_REQUIRED" : !online ? "SESSION_NOT_READY"
            : transfer ? "FILE_TRANSFER_BUSY" : (other || legacyBusy) ? "TERMINAL_BUSY" : "";
        return {{"permissions", QJsonObject{{"readOutput", access->canRead(client, *entry)},
                    {"terminalInput", allowed}, {"useElevation", access->canUseElevation(client, *entry)}}},
            {"availability", QJsonObject{{"inputAvailable", reason.isEmpty()}, {"disabledReason", reason},
                {"ownedByThisClient", controller->owner() == client},
                {"controlState", controller->owner().isEmpty() ? "idle" : "controlled"}}},
            {"shellIntegration", QJsonObject{{"available", controller->integrated()}}},
            {"elevationMethods", QJsonArray{}}}; // No unverified password-prompt adapter.
    }
    std::optional<QJsonObject> token(const QString& encoded, const QString& client,
                                    const SessionDirectory::Entry& entry, const char* kind) {
        const auto result = verifyToken(encoded, key);
        if (!result || result->value("k") != kind || result->value("c") != client
            || result->value("s") != entry.id || result->value("e") != entry.epoch
            || result->value("g").toString() != number(access->version(client))) return std::nullopt;
        return result;
    }
    QJsonObject inputState(const QString& client, const SessionDirectory::Entry& entry) {
        auto* ctrl = entry.session->terminalController();
        const auto replay = replays.value(client + '/' + entry.id);
        return {{"k", "i"}, {"c", client}, {"s", entry.id}, {"e", entry.epoch},
            {"g", number(access->version(client))}, {"u", number(ctrl->userGeneration())},
            {"t", number(ctrl->controlGeneration())}, {"p", number(ctrl->promptGeneration())},
            {"x", ctrl->executionId()}, {"n", number(replay.generation == ctrl->controlGeneration() ? replay.next : 0)},
            {"expires", double(clock.elapsed() + 30000)}};
    }
    bool validInput(const QString& encoded, const QString& client, const SessionDirectory::Entry& entry) {
        const auto value = token(encoded, client, entry, "i");
        if (!value || value->value("expires").toDouble() <= clock.elapsed()) return false;
        auto expected = inputState(client, entry);
        for (const auto& field : {"u", "t", "p", "x", "n"})
            if (value->value(field) != expected.value(field)) return false;
        return true;
    }
    void call(const QString& client, const QString& request, const QString& tool,
              const QJsonObject& args, Completion done) {
        const auto entry = sessions->find(args.value("sessionId").toString());
        if (!entry || !entry->session || !access->canRead(client, *entry)) { done(failure("SESSION_NOT_AVAILABLE", "Session is not shared.")); return; }
        if (entry->epoch != args.value("epoch")) { done(failure("STALE_SESSION_EPOCH", "Refresh the session identity.")); return; }
        watch(entry->session);
        auto* ctrl = entry->session->terminalController();
        if (tool == "novaterm_search_context") { search(client, *entry, args, std::move(done)); return; }
        if (args.contains("expandToken")) { expand(client, *entry, args, std::move(done)); return; }
        bool replacesWait = false;
        for (const auto& old : waiting) if (old->client == client && old->session == entry->id) replacesWait = true;
        if (waiting.size() >= 32 && !replacesWait) { done(failure("BUSY", "Observation waiter budget is full.")); return; }
        auto job = std::make_shared<Pending>();
        job->client = client; job->request = request; job->tool = tool; job->args = args;
        job->session = entry->id; job->epoch = entry->epoch; job->done = std::move(done);
        job->created = clock.elapsed(); job->grant = access->version(client);
        job->interaction = tool != "novaterm_read_context";
        const auto snapshot = entry->session->tryTerminalContext();
        if (!snapshot) { job->done(failure("BUSY", "Snapshot is being prepared.", true, 50)); return; }
        job->since = snapshot->state.revision; job->event = ctrl->revision();
        job->user = ctrl->userGeneration(); job->control = ctrl->controlGeneration();
        int duration = args.value("waitMs").toInt(job->interaction ? (commandAction(args) ? 30000 : 1000) : 0);
        job->mode = args.value("waitFor").toString(job->interaction && !commandAction(args) ? "change" : "attention");
        if (!job->interaction) {
            const auto encoded = args.value("sinceToken").toString();
            job->initial = encoded.isEmpty();
            if (!encoded.isEmpty()) {
                const auto cursor = token(encoded, client, *entry, "r");
                if (!cursor) { job->done(failure("CURSOR_STALE", "Refresh the observation.")); return; }
                job->since = cursor->value("r").toString().toULongLong();
                job->event = cursor->value("v").toString().toULongLong();
            }
        } else {
            const auto availability = capabilities(client, entry->id).value("availability").toObject();
            const auto denied = availability.value("disabledReason").toString();
            if (!denied.isEmpty()) { job->done(failure(denied, denied)); return; }
            if (tool == "novaterm_request_elevation") {
                if (!access->canUseElevation(client, *entry)) job->done(failure("ELEVATION_PERMISSION_REQUIRED", "Managed elevation is not authorized."));
                else if (!validInput(args.value("inputToken").toString(), client, *entry)) job->done(failure("CONTROL_INVALID", "Observe before requesting elevation."));
                else job->done(failure("ELEVATION_ADAPTER_UNAVAILABLE", "No trusted authentication adapter is installed for this connection."));
                return; // Never retrieve or submit a credential through ordinary terminal input.
            }
            const QString replayKey = client + '/' + entry->id;
            const QString operation = args.value("operationId").toString();
            QJsonObject actionIdentity{{"actions", args.value("actions")},
                {"replyToExecutionId", args.value("replyToExecutionId")},
                {"requireVerifiedReady", args.value("requireVerifiedReady")}};
            const auto hash = digest(actionIdentity);
            auto& replay = replays[replayKey];
            if (replay.operation == operation && replay.epoch == entry->epoch && replay.grant == job->grant
                && replay.generation == ctrl->controlGeneration()) {
                if (replay.hash != hash) { job->done(failure("INPUT_SEQUENCE_CONFLICT", "Operation ID was used with other actions.")); return; }
                duration = 0; job->wake = "state_changed";
            } else {
                if (!validInput(args.value("inputToken").toString(), client, *entry)) { job->done(failure("CONTROL_INVALID", "Input context changed; observe again.")); return; }
                const auto actions = args.value("actions").toArray();
                const QString type = actions.first().toObject().value("type").toString();
                const auto reply = args.value("replyToExecutionId").toString();
                if ((!reply.isEmpty() && (!ctrl->commandActive() || ctrl->executionId() != reply))
                    || (ctrl->commandActive() && type != "release" && reply.isEmpty())) {
                    job->done(failure("COMMAND_CONTEXT_CHANGED", "Associate the input with the active command.")); return;
                }
                if (type == "command" && (!ctrl->readyForCommand()
                    || (args.value("requireVerifiedReady").toBool() && !ctrl->integrated()))) {
                    job->done(failure("CONTEXT_NOT_READY", "Observe the current program before submitting a command.")); return;
                }
                if (ctrl->pending()) { job->done(failure("INPUT_BUSY", "The previous input batch is still being encoded.", true, 50)); return; }
                if (type == "release") {
                    ctrl->release(client); duration = 0;
                } else {
                    int controlled = 0;
                    for (const auto& session : sessions->entries()) if (session.session
                        && !session.session->terminalController()->owner().isEmpty()) ++controlled;
                    if (controlled >= 32 && ctrl->owner().isEmpty()) { job->done(failure("BUSY", "Terminal control budget is full.")); return; }
                    if (!ctrl->acquire(client)) { job->done(failure("TERMINAL_BUSY", "Terminal input is owned by another operation.")); return; }
                    if (!ctrl->submit(client, operation, encode(actions, ctrl->lineEnding()), type == "command")) {
                        job->done(failure("INPUT_BUSY", "Input was not accepted.")); return;
                    }
                }
                if (replay.generation != ctrl->controlGeneration()) replay.next = 0;
                replay.generation = ctrl->controlGeneration(); ++replay.next;
                replay.operation = operation; replay.epoch = entry->epoch;
                replay.grant = job->grant; replay.hash = hash;
                deliveredHints.remove(replayKey);
                // Do not return merely because admission changed the state to queued/submitted.
                job->event = ctrl->revision(); job->control = ctrl->controlGeneration();
            }
        }
        for (const auto& old : waiting) if (old->client == client && old->session == entry->id) {
            old->deadline = clock.elapsed(); old->wake = "state_changed";
        }
        job->deadline = clock.elapsed() + duration;
        if (job->initial) job->deadline = clock.elapsed();
        waiting.append(job); ctrl->touch(client);
        if (!poll.isActive()) poll.start();
        QTimer::singleShot(0, q, [this] { flush(); });
    }

    std::vector<NovaTerm::TerminalInputAction> encode(const QJsonArray& actions, const QByteArray& ending) {
        std::vector<NovaTerm::TerminalInputAction> result;
        const QStringList keys{"", "Enter", "Tab", "Backspace", "Escape", "Up", "Down", "Left", "Right",
            "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        for (const auto& value : actions) {
            const auto action = value.toObject(); const auto type = action.value("type").toString();
            NovaTerm::TerminalInputAction input;
            if (type == "command" || type == "text" || type == "paste") {
                input.kind = type == "paste" ? NovaTerm::TerminalInputAction::Kind::Paste : NovaTerm::TerminalInputAction::Kind::Text;
                input.text = action.value(type == "command" ? "command" : "text").toString().toUtf8().toStdString();
            } else if (type == "line_ending") {
                input.kind = NovaTerm::TerminalInputAction::Kind::Bytes; input.text = ending.toStdString();
            } else {
                input.kind = NovaTerm::TerminalInputAction::Kind::Key;
                const auto name = action.value("key").toString();
                const auto index = keys.indexOf(name);
                if (index > 0) input.key = static_cast<NovaTerm::InputKey>(index);
                else input.text = name.toUtf8().toStdString();
                for (const auto& modifier : action.value("modifiers").toArray())
                    input.modifiers |= modifier == "Ctrl" ? 4U : modifier == "Alt" ? 2U : 1U;
            }
            result.push_back(std::move(input));
            if (type == "command") {
                NovaTerm::TerminalInputAction enter;
                enter.kind = NovaTerm::TerminalInputAction::Kind::Bytes;
                enter.text = ending.toStdString(); result.push_back(std::move(enter));
            }
        }
        return result;
    }

    void flush() {
        if (flushing) return;
        flushing = true;
        const auto copy = waiting;
        for (const auto& job : copy) {
            if (!waiting.contains(job)) continue;
            const auto entry = sessions->find(job->session);
            if (!entry || !entry->session || !access->canRead(job->client, *entry)
                || entry->epoch != job->epoch || access->version(job->client) != job->grant) {
                finish(job, failure("SESSION_NOT_AVAILABLE", "The observation permission or connection changed.")); continue;
            }
            auto* ctrl = entry->session->terminalController();
            const auto snapshot = entry->session->tryTerminalContext();
            if (!snapshot) {
                if (clock.elapsed() >= job->deadline) finish(job, failure("BUSY", "Snapshot is being prepared.", true, 50));
                continue;
            }
            const auto revision = snapshot->state.revision;
            if (lastModel.value(entry->id) != revision) { lastModel[entry->id] = revision; lastOutput[entry->id] = clock.elapsed(); }
            QString wake = job->wake;
            const auto command = ctrl->observation(job->client).value("command").toObject();
            const bool event = job->event != ctrl->revision();
            const bool model = job->since != revision;
            if (wake.isEmpty() && (ctrl->userGeneration() != job->user || ctrl->controlGeneration() != job->control
                || entry->state != SessionState::Running)) wake = "control_changed";
            if (wake.isEmpty() && event && command.value("state") == "finished") wake = "command_finished";
            if (wake.isEmpty() && job->mode == "change" && (model || event)
                && clock.elapsed() - lastOutput.value(entry->id) >= 150) wake = "output_changed";
            if (wake.isEmpty() && job->mode == "attention" && model
                && clock.elapsed() - lastOutput.value(entry->id) >= 150) wake = attention(*job, *snapshot);
            if (wake.isEmpty() && clock.elapsed() >= job->deadline) wake = job->initial ? "initial" : "deadline";
            if (!wake.isEmpty()) {
                ctrl->touch(job->client);
                finish(job, project(*job, *entry, *snapshot, wake));
            }
        }
        flushing = false;
        if (waiting.isEmpty()) poll.stop();
    }
    QString attention(const Pending& job, const TerminalContextProvider::Snapshot& snapshot) {
        QString last;
        for (const auto& line : snapshot.state.viewport) {
            const auto text = QString::fromUtf8(line.text.data(), qsizetype(line.text.size()));
            if (!text.trimmed().isEmpty()) last = text;
        }
        static const QRegularExpression prompt(QStringLiteral("(?i)(\\[[yn]/[yn]\\]|\\([yn]/[yn]\\)|(?:password|passphrase):\\s*$|(?:continue|proceed).{0,32}\\?|press .{0,32}key)"));
        static const QRegularExpression error(QStringLiteral("(?i)^(?:fatal error:|error:|traceback|panic:)|: fatal error:"));
        const QString type = prompt.match(last).hasMatch() ? "attention_candidate"
            : error.match(last).hasMatch() ? "diagnostic_candidate" : "";
        if (type.isEmpty()) return {};
        const auto fingerprint = QCryptographicHash::hash((type + last).toUtf8(), QCryptographicHash::Sha256);
        auto& hints = deliveredHints[job.client + '/' + job.session];
        if (hints.contains(fingerprint) || hints.size() >= 4) return {};
        hints.insert(fingerprint); return type;
    }
    void finish(const std::shared_ptr<Pending>& job, const QJsonObject& result) {
        waiting.removeAll(job); job->done(result);
    }

    QJsonObject project(const Pending& job, const SessionDirectory::Entry& entry,
                        const TerminalContextProvider::Snapshot& snapshot, const QString& wake) {
        auto* ctrl = entry.session->terminalController();
        const bool compact = job.args.value("responseMode").toString(job.interaction ? "compact" : "full") == "compact";
        const bool changed = job.since != snapshot.state.revision;
        QJsonObject data = ctrl->observation(job.client);
        data.insert("sessionId", entry.id); data.insert("epoch", entry.epoch);
        data.insert("observationId", newId()); data.insert("modelRevision", number(snapshot.state.revision));
        data.insert("eventRevision", number(ctrl->revision())); data.insert("sessionState", SessionDirectory::stateName(entry.state));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(snapshot.capturedAt.time_since_epoch()).count();
        data.insert("capturedAt", QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs));
        const auto size = entry.session->core()->screenSize();
        data.insert("screenSize", QJsonObject{{"columns", size.first}, {"rows", size.second}});
        data.insert("wakeReason", wake); data.insert("outputChanged", changed); data.insert("screenChanged", changed);
        data.insert("alternateScreen", snapshot.state.alternateScreen);
        data.insert("cursor", QJsonObject{{"row", snapshot.state.cursor.position.row}, {"column", snapshot.state.cursor.position.col}});
        data.insert("operation", job.interaction ? QJsonValue(ctrl->operation()) : QJsonValue(QJsonValue::Undefined));
        QJsonObject cursor{{"k", "r"}, {"c", job.client}, {"s", entry.id}, {"e", entry.epoch},
            {"g", number(job.grant)}, {"r", number(snapshot.state.revision)}, {"v", number(ctrl->revision())}};
        data.insert("nextToken", signToken(cursor, key));
        const bool canInput = capabilities(job.client, entry.id).value("availability").toObject().value("inputAvailable").toBool();
        if (canInput) data.insert("inputToken", signToken(inputState(job.client, entry), key));
        auto readiness = data.value("inputReadiness").toObject();
        readiness.insert("canInteract", canInput && readiness.value("canInteract").toBool());
        readiness.insert("canSubmitCommand", canInput && readiness.value("canSubmitCommand").toBool());
        data.insert("inputReadiness", readiness);
        QJsonArray lines;
        if (!job.initial && changed && !snapshot.state.alternateScreen) {
            for (const auto& line : snapshot.entries) if (line.revision > job.since)
                lines.append(QJsonObject{{"text", QString::fromUtf8(line.text.data(), qsizetype(line.text.size()))}, {"source", "recent_output"}});
        }
        const bool viewport = job.args.value("includeViewport").toBool(job.initial || snapshot.state.alternateScreen || lines.isEmpty());
        if (viewport && (job.initial || changed)) {
            lines = {}; int row = 0;
            for (const auto& line : snapshot.state.viewport) {
                lines.append(QJsonObject{{"text", QString::fromUtf8(line.text.data(), qsizetype(line.text.size()))}, {"row", row++}, {"source", "viewport"}});
            }
        }
        const bool gap = snapshot.state.truncated || (!job.initial && snapshot.cacheFloor > 0 && job.since <= snapshot.cacheFloor)
            || job.since > snapshot.state.revision || job.since < snapshot.resetRevision;
        data.insert("sourceGap", gap); data.insert("sourceTruncated", snapshot.state.truncated);
        data.insert("resetRequired", gap);
        data.insert("viewportPartial", compact && viewport);
        const QString captureId = newId();
        if (!lines.isEmpty()) {
            if (captures.size() >= 32) {
                auto oldest = captures.begin();
                for (auto it = captures.begin(); it != captures.end(); ++it) if (it->expires < oldest->expires) oldest = it;
                captures.erase(oldest);
            }
            QStringList owned;
            for (auto it = captures.begin(); it != captures.end(); ++it) if (it->client == job.client) owned.append(it.key());
            if (owned.size() >= 4) captures.remove(owned.first());
            captures.insert(captureId, Capture{job.client, entry.id, entry.epoch, job.grant, clock.elapsed()+60000,
                lines, number(snapshot.state.revision), gap});
        }
        data.insert("captureId", captureId);
        return fit(data, lines, compact, job.args, captureId, job.client, entry, false);
    }
    QJsonObject fit(QJsonObject data, const QJsonArray& all, bool compact, const QJsonObject& args,
                   const QString& id, const QString& client, const SessionDirectory::Entry& entry,
                   bool expansion, int offset = 0, int byteOffset = 0) {
        int budget = args.value("maxBytes").toInt(compact ? 4096 : 65536);
        const int maxLines = args.value("maxLines").toInt(compact ? 24 : 256);
        const int end = int(all.size());
        int start = expansion ? offset : (std::max)(0, end - maxLines);
        QJsonArray selected;
        QList<QPair<int, int>> starts;
        int position = start;
        int nextByte = expansion ? byteOffset : 0;
        bool clippedText = false;
        for (; position < end && selected.size() < maxLines;) {
            auto line = all[position].toObject(); const auto raw = line.value("text").toString().toUtf8();
            int from = expansion ? nextByte : 0;
            if (raw.size() - from > budget && !selected.isEmpty()) break;
            if (!expansion && raw.size() > budget) {
                from = int(raw.size()) - budget;
                while (from < raw.size() && (static_cast<unsigned char>(raw[from]) & 0xc0) == 0x80) ++from;
            }
            const auto clipped = clipUtf8(raw.mid(from), budget);
            starts.append({position, from});
            line.insert("text", QString::fromUtf8(clipped));
            if (from || clipped.size() != raw.size()) line.insert("partial", true);
            budget -= int(clipped.size()); selected.append(line);
            nextByte = from + int(clipped.size());
            if (nextByte < raw.size()) { clippedText = true; break; }
            if (from) clippedText = true;
            ++position; nextByte = 0;
            if (budget <= 0) break;
        }
        const auto install = [&] {
            QJsonArray viewport, recent;
            for (const auto& item : selected) {
                auto line = item.toObject(); const auto source = line.take("source").toString();
                if (source == "viewport") viewport.append(line); else recent.append(line);
            }
            data.insert("viewport", viewport); data.insert("recentOutput", recent);
            const bool hasMore = expansion ? position < end : (start > 0 || position < end || clippedText);
            data.insert("projectionLimited", hasMore);
            data.insert("omittedLines", (std::max)(0, int(all.size() - selected.size())));
            data.insert("outputTruncated", data.value("projectionLimited"));
            if (!all.isEmpty() && data.value("projectionLimited").toBool()) {
                const int next = expansion ? position : 0;
                data.insert("expandToken", signToken({{"k", "z"}, {"c", client}, {"s", entry.id}, {"e", entry.epoch},
                    {"g", number(access->version(client))}, {"id", id}, {"offset", next},
                    {"byte", expansion ? nextByte : 0}}, key));
            } else data.insert("expandToken", QJsonValue(QJsonValue::Null));
        };
        install();
        while (compact && QJsonDocument(toolResult(success(data))).toJson(QJsonDocument::Compact).size() > 8192 && !selected.isEmpty()) {
            if (expansion) {
                position = starts.last().first; nextByte = starts.last().second;
                starts.removeLast(); selected.removeLast();
            } else { selected.removeFirst(); starts.removeFirst(); ++start; }
            install();
        }
        if (compact && QJsonDocument(toolResult(success(data))).toJson(QJsonDocument::Compact).size() > 8192)
            return failure("RESPONSE_METADATA_LIMIT", "Observation metadata exceeds the compact budget.");
        return success(data);
    }
    void expand(const QString& client, const SessionDirectory::Entry& entry, const QJsonObject& args, Completion done) {
        const auto value = token(args.value("expandToken").toString(), client, entry, "z");
        const auto captured = value ? captures.value(value->value("id").toString()) : Capture{};
        if (!value || captured.client != client || captured.session != entry.id || captured.epoch != entry.epoch
            || captured.grant != access->version(client) || captured.expires <= clock.elapsed()) {
            done(failure("CAPTURE_EXPIRED", "The bounded capture is no longer available.")); return;
        }
        QJsonObject data{{"sessionId", entry.id}, {"epoch", entry.epoch}, {"captureId", value->value("id")}, {"wakeReason", "initial"}};
        done(fit(data, captured.lines, args.value("responseMode").toString("compact") == "compact", args,
            value->value("id").toString(), client, entry, true, value->value("offset").toInt(), value->value("byte").toInt()));
    }
    void search(const QString& client, const SessionDirectory::Entry& entry, const QJsonObject& args, Completion done) {
        const auto captured = captures.value(args.value("captureId").toString());
        if (captured.client != client || captured.session != entry.id || captured.epoch != entry.epoch
            || captured.grant != access->version(client) || captured.expires <= clock.elapsed()) {
            done(failure("CAPTURE_NOT_AVAILABLE", "Capture is not available.")); return;
        }
        const auto query = args.value("query").toString().toUtf8();
        const bool sensitive = args.value("caseSensitive").toBool(true);
        const auto needle = sensitive ? query : query.toLower();
        const auto scope = args.value("scope").toString("both");
        const int limit = args.value("maxMatches").toInt(20);
        QJsonArray matches; bool limited = false; int screen = 0, recent = 0;
        for (const auto& item : captured.lines) {
            const auto line = item.toObject(); const auto source = line.value("source").toString();
            const int index = source == "viewport" ? screen++ : recent++;
            if (scope != "both" && scope != source) continue;
            const auto original = line.value("text").toString().toUtf8();
            const auto text = sensitive ? original : original.toLower();
            qsizetype offset = 0;
            while ((offset = text.indexOf(needle, offset)) >= 0) {
                if (matches.size() >= limit) { limited = true; break; }
                matches.append(QJsonObject{{"source", source}, {"lineIndex", index},
                    {"startByte", double(offset)}, {"endByte", double(offset + needle.size())}});
                offset += needle.size();
            }
            if (limited) break;
        }
        done(success({{"captureId", args.value("captureId")}, {"revision", captured.revision}, {"matches", matches},
            {"limited", limited}, {"sourceTruncated", captured.sourceTruncated}, {"outputTruncated", false}}));
    }
    void cancel(const QString& request) {
        const auto copy = waiting;
        for (const auto& job : copy) if (job->request == request) waiting.removeAll(job);
        if (waiting.isEmpty()) poll.stop();
    }
    void revoke(const QString& client, const QString& session) {
        for (const auto& entry : sessions->entries()) if ((session.isEmpty() || session == entry.id) && entry.session) {
            auto* ctrl = entry.session->terminalController();
            if (client.isEmpty() || ctrl->owner() == client) ctrl->release(client, "revoked");
        }
        const auto copy = waiting;
        for (const auto& job : copy) if ((client.isEmpty() || job->client == client) && (session.isEmpty() || job->session == session))
            finish(job, failure("SESSION_NOT_AVAILABLE", "Access or connection changed."));
        for (auto it = captures.begin(); it != captures.end();) {
            if ((client.isEmpty() || it->client == client) && (session.isEmpty() || it->session == session)) it = captures.erase(it); else ++it;
        }
        for (auto it = replays.begin(); it != replays.end();) {
            if ((client.isEmpty() || it.key().startsWith(client + '/')) && (session.isEmpty() || it.key().endsWith('/' + session))) it = replays.erase(it); else ++it;
        }
        for (auto it = deliveredHints.begin(); it != deliveredHints.end();) {
            if ((client.isEmpty() || it.key().startsWith(client + '/')) && (session.isEmpty() || it.key().endsWith('/' + session))) it = deliveredHints.erase(it); else ++it;
        }
        if (!session.isEmpty()) { lastModel.remove(session); lastOutput.remove(session); }
    }
};

TerminalInteraction::TerminalInteraction(AccessStore* access, SessionDirectory* sessions, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, access, sessions)) {}
TerminalInteraction::~TerminalInteraction() = default;
void TerminalInteraction::call(const QString& c, const QString& r, const QString& t, const QJsonObject& a, Completion done) { _impl->call(c,r,t,a,std::move(done)); }
void TerminalInteraction::cancel(const QString& request) { _impl->cancel(request); }
void TerminalInteraction::revoke(const QString& client, const QString& session) { _impl->revoke(client,session); }
QJsonObject TerminalInteraction::capabilities(const QString& client, const QString& session) { return _impl->capabilities(client,session); }
bool TerminalInteraction::ownsCapture(const QString& client, const QString& capture) const {
    return _impl->captures.contains(capture) && _impl->captures.value(capture).client == client;
}
}
