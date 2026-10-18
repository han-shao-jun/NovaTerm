#include "TerminalInteraction.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "CapabilityAdapter.h"
#include "ObservationService.h"
#include "mcp/contracts/Budget.h"
#include "protocol/ProtocolProjectionSizer.h"
#include "protocol/ToolCodec.h"
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
bool commandAction(const ToolRequest& request)
{
    if (const auto* value = std::get_if<InteractTerminalRequest>(&request))
        return !value->actions.empty()
            && std::holds_alternative<CommandAction>(value->actions.front());
    return false;
}

std::optional<std::chrono::milliseconds> waitOf(const ToolRequest& request)
{
    return std::visit([](const auto& value) -> std::optional<std::chrono::milliseconds> {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ReadContextRequest>
            || std::is_same_v<Type, InteractTerminalRequest>
            || std::is_same_v<Type, RequestElevationRequest>)
            return value.wait;
        return std::nullopt;
    }, request);
}

QString waitMode(const ToolRequest& request, bool interaction)
{
    std::optional<WaitCondition> condition;
    std::visit([&condition](const auto& value) {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ReadContextRequest>
            || std::is_same_v<Type, InteractTerminalRequest>
            || std::is_same_v<Type, RequestElevationRequest>)
            condition = value.waitFor;
    }, request);
    if (condition)
        return *condition == WaitCondition::Change ? QStringLiteral("change")
            : *condition == WaitCondition::Completion ? QStringLiteral("completion")
            : QStringLiteral("attention");
    return interaction && !commandAction(request)
        ? QStringLiteral("change") : QStringLiteral("attention");
}

std::optional<ProjectionMode> projectionOf(const ToolRequest& request)
{
    return std::visit([](const auto& value) -> std::optional<ProjectionMode> {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ReadContextRequest>
            || std::is_same_v<Type, InteractTerminalRequest>
            || std::is_same_v<Type, RequestElevationRequest>)
            return value.projection;
        return std::nullopt;
    }, request);
}

std::optional<bool> includeViewportOf(const ToolRequest& request)
{
    if (const auto* value = std::get_if<ReadContextRequest>(&request))
        return value->includeViewport;
    return std::nullopt;
}

std::optional<int> maxBytesOf(const ToolRequest& request)
{
    if (const auto* value = std::get_if<ReadContextRequest>(&request))
        return value->maxBytes;
    return std::nullopt;
}

std::optional<int> maxLinesOf(const ToolRequest& request)
{
    if (const auto* value = std::get_if<ReadContextRequest>(&request))
        return value->maxLines;
    return std::nullopt;
}
}

class TerminalInteraction::Impl {
public:
    struct Replay {
        quint64 generation{0}, next{0}; QString operation, epoch;
        QByteArray hash; quint64 grant{0};
    };
    struct Pending {
        QString client, request, tool, session, epoch, mode, wake;
        ToolRequest typed; Completion done;
        quint64 since{0}, event{0}, user{0}, control{0}, grant{0};
        qint64 deadline{0}, created{0}; bool initial{false}, interaction{false};
        bool completed{false};
    };
    Impl(TerminalInteraction* owner, AccessStore* store, SessionDirectory* directory,
         ObservationService* observationService)
        : q(owner), access(store), sessions(directory), observation(observationService),
          key(randomBytes()) {
        clock.start(); poll.setInterval(250);
        wake.setSingleShot(true);
        QObject::connect(&wake, &QTimer::timeout, q, [this] { flush(); });
        QObject::connect(&poll, &QTimer::timeout, q, [this] { flush(); });
        QObject::connect(sessions, &SessionDirectory::invalidated, q,
            [this](const QString& id) { revoke({}, id); });
    }
    TerminalInteraction* q;
    AccessStore* access;
    SessionDirectory* sessions;
    ObservationService* observation;
    QByteArray key;
    QElapsedTimer clock;
    QTimer poll;
    QTimer wake;
    QList<std::shared_ptr<Pending>> waiting;
    QHash<QString, Replay> replays;
    QHash<QString, quint64> lastModel;
    QHash<QString, qint64> lastOutput;
    QHash<QString, QSet<QByteArray>> deliveredHints;
    QSet<TerminalSession*> watched;
    bool flushing{false};

    void watch(TerminalSession* session) {
        if (watched.contains(session)) return;
        watched.insert(session);
        QObject::connect(session, &TerminalSession::activityChanged, q, [this, session] {
            const auto id = session->id().toString(QUuid::WithoutBraces);
            const auto revision = session->core()->modelRevision();
            if (lastModel.value(id) != revision) {
                lastModel[id] = revision;
                lastOutput[id] = clock.elapsed();
            }
            scheduleFlush();
        });
        QObject::connect(session, &TerminalSession::terminalControlChanged, q, [this] {
            if (q->onChanged) q->onChanged();
        });
        QObject::connect(session, &QObject::destroyed, q, [this, session] { watched.remove(session); });
    }
    void scheduleFlush() {
        if (!waiting.isEmpty() && !wake.isActive()) wake.start(0);
    }
    std::shared_ptr<const TerminalContextProvider::Snapshot> baseSnapshot(
        const SessionDirectory::Entry& entry) {
        return observation->snapshot(entry.id, entry.epoch,
            [&entry] { return entry.session->tryTerminalContext(); });
    }
    QJsonObject capabilities(const QString& client, const QString& id,
                             bool humanConfirmation = false) {
        const auto entry = sessions->find(id);
        if (!entry || !entry->session) return {};
        const auto capability = sessionCapabilities(*entry, client, *access,
            humanConfirmation);
        QJsonArray capabilityNames;
        for (const auto& name : capability.capabilities)
            capabilityNames.append(name);
        return {{"capabilities", capabilityNames},
            {"permissions", QJsonObject{
                    {"readOutput", capability.permissions.read},
                    {"terminalInput", capability.terminalInputAvailable},
                    {"useElevation", capability.terminalInputAvailable
                        && capability.permissions.elevation}}},
            {"availability", QJsonObject{
                {"inputAvailable", capability.terminalInputAvailable},
                {"disabledReason", capability.terminalInputDisabledReason},
                {"elevationAvailable", capability.elevationAvailable},
                {"elevationDisabledReason", capability.elevationDisabledReason},
                {"ownedByThisClient", capability.ownedByThisClient},
                {"controlState", capability.terminalOwner.isEmpty()
                    ? "idle" : "controlled"}}},
            {"shellIntegration", QJsonObject{
                {"available", capability.shellIntegrationAvailable}}},
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
            {"expires", double(clock.elapsed() + Budget::MaxTerminalWaitMs)}};
    }
    QString inputError(const QString& encoded, const QString& client, const SessionDirectory::Entry& entry) {
        const auto value = token(encoded, client, entry, "i");
        if (!value) return QStringLiteral("CONTROL_INVALID");
        if (value->value("expires").toDouble() <= clock.elapsed()) return QStringLiteral("INPUT_TOKEN_EXPIRED");
        auto expected = inputState(client, entry);
        for (const auto& field : {"u", "t", "p", "x"})
            if (value->value(field) != expected.value(field)) return QStringLiteral("CONTROL_INVALID");
        const auto sequence = value->value("n").toString().toULongLong();
        const auto next = expected.value("n").toString().toULongLong();
        if (sequence < next) return QStringLiteral("INPUT_ALREADY_SUBMITTED");
        if (sequence > next) return QStringLiteral("INPUT_OUT_OF_ORDER");
        return {};
    }
    void call(const QString& client, const QString& request, const ToolRequest& typed,
              Completion done, const QByteArray& legacyCursorKey) {
        const auto reference = sessionReference(typed);
        if (!reference) { done(failure("SESSION_NOT_AVAILABLE", "Session is not shared.")); return; }
        const auto entry = sessions->find(reference->session.str());
        if (!entry || !entry->session || !access->canRead(client, *entry)) { done(failure("SESSION_NOT_AVAILABLE", "Session is not shared.")); return; }
        if (entry->epoch != reference->epoch.str()) { done(failure("STALE_SESSION_EPOCH", "Refresh the session identity.")); return; }
        watch(entry->session);
        auto* ctrl = entry->session->terminalController();
        const QString tool = ToolCodec::toolName(typed);
        if (const auto* searchRequest = std::get_if<SearchContextRequest>(&typed)) {
            search(client, *entry, *searchRequest, std::move(done));
            return;
        }
        const auto* readRequest = std::get_if<ReadContextRequest>(&typed);
        if (readRequest && readRequest->expandToken) {
            expand(client, *entry, *readRequest, std::move(done));
            return;
        }
        bool replacesWait = false;
        for (const auto& old : waiting) if (old->client == client && old->session == entry->id) replacesWait = true;
        if (waiting.size() >= Budget::MaxObservationWaiters && !replacesWait) { done(failure("BUSY", "Observation waiter budget is full.")); return; }
        auto job = std::make_shared<Pending>();
        job->client = client; job->request = request; job->tool = tool; job->typed = typed;
        job->session = entry->id; job->epoch = entry->epoch; job->done = std::move(done);
        job->created = clock.elapsed(); job->grant = access->version(client);
        job->interaction = tool != "novaterm_read_context";
        const auto snapshot = baseSnapshot(*entry);
        if (!snapshot) { job->done(failure("BUSY", "Snapshot is being prepared.", true, 50)); return; }
        job->since = snapshot->state.revision; job->event = ctrl->revision();
        job->user = ctrl->userGeneration(); job->control = ctrl->controlGeneration();
        const auto wait = waitOf(typed);
        int duration = wait ? int(wait->count())
            : job->interaction ? (commandAction(typed) ? Budget::MaxTerminalWaitMs : 1000) : 0;
        job->mode = waitMode(typed, job->interaction);
        if (!job->interaction) {
            const auto encoded = readRequest->sinceToken.value_or(QString());
            job->initial = encoded.isEmpty();
            if (!encoded.isEmpty()) {
                const auto cursor = token(encoded, client, *entry, "r");
                if (cursor) {
                    job->since = cursor->value("r").toString().toULongLong();
                    job->event = cursor->value("v").toString().toULongLong();
                } else {
                    // A client may opt into compact/waiting after a legacy read.
                    // Verify its original connection-bound cursor before migrating.
                    const auto legacy = legacyCursorKey.isEmpty() ? std::nullopt
                        : verifyToken(encoded, legacyCursorKey);
                    bool validRevision = false;
                    const auto revision = legacy
                        ? legacy->value("revision").toString().toULongLong(&validRevision) : 0;
                    if (!legacy || !validRevision || legacy->value("kind") != "context"
                        || legacy->value("session") != entry->id || legacy->value("epoch") != entry->epoch
                        || legacy->value("grant").toString() != number(job->grant)
                        || legacy->value("viewport").toBool() != readRequest->includeViewport.value_or(true)) {
                        job->done(failure("CURSOR_STALE", "Refresh the observation.")); return;
                    }
                    job->since = revision;
                }
            }
        } else {
            const auto availability = capabilities(client, entry->id).value("availability").toObject();
            const auto denied = availability.value("disabledReason").toString();
            if (!denied.isEmpty()) { job->done(failure(denied, denied)); return; }
            const auto* elevation = std::get_if<RequestElevationRequest>(&typed);
            const auto* interaction = std::get_if<InteractTerminalRequest>(&typed);
            if (elevation) {
                if (!access->canUseElevation(client, *entry)) job->done(failure("ELEVATION_PERMISSION_REQUIRED", "Managed elevation is not authorized."));
                else if (const auto error = inputError(elevation->inputToken, client, *entry); !error.isEmpty())
                    job->done(failure(error, "Observe before requesting elevation."));
                else job->done(failure("ELEVATION_ADAPTER_UNAVAILABLE", "No trusted authentication adapter is installed for this connection."));
                return; // Never retrieve or submit a credential through ordinary terminal input.
            }
            const QString replayKey = client + '/' + entry->id;
            const QString operation = interaction->operation.str();
            const QString reply = interaction->replyToExecution
                ? interaction->replyToExecution->str() : QString();
            QJsonObject actionIdentity{{"actions", ToolCodec::encodeActions(interaction->actions)},
                {"replyToExecutionId", reply},
                {"requireVerifiedReady", interaction->requireVerifiedReady}};
            const auto hash = digest(actionIdentity);
            auto& replay = replays[replayKey];
            if (replay.operation == operation && replay.epoch == entry->epoch && replay.grant == job->grant
                && replay.generation == ctrl->controlGeneration()) {
                if (replay.hash != hash) { job->done(failure("INPUT_SEQUENCE_CONFLICT", "Operation ID was used with other actions.")); return; }
                duration = 0; job->wake = "state_changed";
            } else {
                const auto error = inputError(interaction->inputToken, client, *entry);
                if (!error.isEmpty()) { job->done(failure(error, "Input context changed; observe again.")); return; }
                const auto& actions = interaction->actions;
                const bool release = !actions.empty()
                    && std::holds_alternative<ReleaseAction>(actions.front());
                const bool command = commandAction(typed);
                if ((!reply.isEmpty() && (!ctrl->commandActive() || ctrl->executionId() != reply))
                    || (ctrl->commandActive() && !release && reply.isEmpty())) {
                    job->done(failure("COMMAND_CONTEXT_CHANGED", "Associate the input with the active command.")); return;
                }
                if (command && (!ctrl->readyForCommand()
                    || (interaction->requireVerifiedReady && !ctrl->integrated()))) {
                    job->done(failure("CONTEXT_NOT_READY", "Observe the current program before submitting a command.")); return;
                }
                if (ctrl->pending()) { job->done(failure("INPUT_BUSY", "The previous input batch is still being encoded.", true, 50)); return; }
                if (release) {
                    ctrl->release(client, QStringLiteral("released"), operation); duration = 0;
                } else {
                    int controlled = 0;
                    for (const auto& session : sessions->entries()) if (session.session
                        && !session.session->terminalController()->owner().isEmpty()) ++controlled;
                    if (controlled >= Budget::MaxObservedControlSessions && ctrl->owner().isEmpty()) { job->done(failure("BUSY", "Terminal control budget is full.")); return; }
                    if (!ctrl->acquire(client)) { job->done(failure("TERMINAL_BUSY", "Terminal input is owned by another operation.")); return; }
                    if (!ctrl->submit(client, operation, encode(actions, ctrl->lineEnding()),
                        command, int(actions.size()))) {
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
        scheduleFlush();
    }

    std::vector<NovaTerm::TerminalInputAction> encode(
        const std::vector<TerminalAction>& actions, const QByteArray& ending) {
        std::vector<NovaTerm::TerminalInputAction> result;
        const QStringList keys{"", "Enter", "Tab", "Backspace", "Escape", "Up", "Down", "Left", "Right",
            "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        for (const auto& action : actions) {
            NovaTerm::TerminalInputAction input;
            bool command = false;
            std::visit([&](const auto& value) {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, TextAction>) {
                    input.kind = NovaTerm::TerminalInputAction::Kind::Text;
                    input.text = value.text.toUtf8().toStdString();
                } else if constexpr (std::is_same_v<Type, PasteAction>) {
                    input.kind = NovaTerm::TerminalInputAction::Kind::Paste;
                    input.text = value.text.toUtf8().toStdString();
                } else if constexpr (std::is_same_v<Type, CommandAction>) {
                    command = true;
                    input.kind = NovaTerm::TerminalInputAction::Kind::Text;
                    input.text = value.command.toUtf8().toStdString();
                } else if constexpr (std::is_same_v<Type, LineEndingAction>) {
                    input.kind = NovaTerm::TerminalInputAction::Kind::Bytes;
                    input.text = ending.toStdString();
                } else if constexpr (std::is_same_v<Type, KeyAction>) {
                    input.kind = NovaTerm::TerminalInputAction::Kind::Key;
                    const auto index = keys.indexOf(value.key);
                    if (index > 0) input.key = static_cast<NovaTerm::InputKey>(index);
                    else input.text = value.key.toUtf8().toStdString();
                    for (const auto modifier : value.modifiers)
                        input.modifiers |= modifier == KeyModifier::Ctrl ? 4U
                            : modifier == KeyModifier::Alt ? 2U : 1U;
                }
            }, action);
            result.push_back(std::move(input));
            if (command) {
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
            if (job->completed || !waiting.contains(job)) continue;
            if (!entry || !entry->session || !access->canRead(job->client, *entry)
                || entry->epoch != job->epoch || access->version(job->client) != job->grant) {
                finish(job, failure("SESSION_NOT_AVAILABLE", "The observation permission or connection changed.")); continue;
            }
            auto* ctrl = entry->session->terminalController();
            const auto snapshot = baseSnapshot(*entry);
            if (!snapshot) {
                if (clock.elapsed() >= job->deadline) finish(job, failure("BUSY", "Snapshot is being prepared.", true, 50));
                continue;
            }
            const auto revision = snapshot->state.revision;
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
            if (!wake.isEmpty() && !job->completed && waiting.contains(job)) {
                ctrl->touch(job->client);
                finish(job, project(*job, *entry, *snapshot, wake));
            }
        }
        flushing = false;
        if (waiting.isEmpty()) poll.stop();
    }
    QString attention(const Pending& job, const TerminalContextProvider::Snapshot& snapshot) {
        auto& hints = deliveredHints[job.client + '/' + job.session];
        if (hints.size() >= 4) return {};
        QString last;
        for (auto it = snapshot.state.viewport.crbegin();
             it != snapshot.state.viewport.crend(); ++it) {
            const auto text = QString::fromUtf8(it->text.data(), qsizetype(it->text.size()));
            if (!text.trimmed().isEmpty()) { last = text; break; }
        }
        static const QRegularExpression prompt(QStringLiteral("(?i)(\\[[yn]/[yn]\\]|\\([yn]/[yn]\\)|(?:password|passphrase):\\s*$|(?:continue|proceed).{0,32}\\?|press .{0,32}key)"));
        static const QRegularExpression error(QStringLiteral("(?i)^(?:fatal error:|error:|traceback|panic:)|: fatal error:"));
        const QString type = prompt.match(last).hasMatch() ? "attention_candidate"
            : error.match(last).hasMatch() ? "diagnostic_candidate" : "";
        if (type.isEmpty()) return {};
        const auto fingerprint = QCryptographicHash::hash((type + last).toUtf8(), QCryptographicHash::Sha256);
        if (hints.contains(fingerprint) || hints.size() >= 4) return {};
        hints.insert(fingerprint); return type;
    }
    void finish(const std::shared_ptr<Pending>& job, const QJsonObject& result) {
        if (job->completed) return;
        job->completed = true;
        waiting.removeAll(job);
        job->done(result);
    }

    QJsonObject project(const Pending& job, const SessionDirectory::Entry& entry,
                        const TerminalContextProvider::Snapshot& snapshot, const QString& wake) {
        auto* ctrl = entry.session->terminalController();
        const bool compact = projectionOf(job.typed).value_or(
            job.interaction ? ProjectionMode::Compact : ProjectionMode::Full)
            == ProjectionMode::Compact;
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
        const auto viewportOption = includeViewportOf(job.typed);
        const bool explicitViewport = viewportOption.value_or(false);
        const bool viewport = viewportOption.value_or(
            !compact || job.initial || snapshot.state.alternateScreen || lines.isEmpty());
        if (viewport && (!compact || explicitViewport || job.initial || changed)) {
            const auto recent = std::move(lines);
            lines = {}; int row = 0;
            for (const auto& line : snapshot.state.viewport) {
                lines.append(QJsonObject{{"text", QString::fromUtf8(line.text.data(), qsizetype(line.text.size()))}, {"row", row++}, {"source", "viewport"}});
            }
            if (!compact) for (const auto& line : recent) lines.append(line);
        }
        const bool gap = snapshot.state.truncated || (!job.initial && snapshot.cacheFloor > 0 && job.since <= snapshot.cacheFloor)
            || job.since > snapshot.state.revision || job.since < snapshot.resetRevision;
        data.insert("sourceGap", gap); data.insert("sourceTruncated", snapshot.state.truncated);
        data.insert("resetRequired", gap);
        data.insert("viewportPartial", compact && viewport);
        const QString captureId = newId();
        if (!lines.isEmpty()) {
            const auto now = clock.elapsed();
            observation->captures().sweep(now);
            CaptureRecord record;
            record.scope = CaptureScope::Client;
            record.owner = job.client;
            record.session = entry.id;
            record.epoch = entry.epoch;
            record.grantVersion = job.grant;
            record.revision = number(snapshot.state.revision);
            record.lines.reserve(lines.size());
            for (const auto& item : lines) {
                const auto line = item.toObject();
                record.lines.append(CaptureLine{line.value("text").toString(),
                    line.value("source").toString(), line.value("row").toInt()});
            }
            record.sourceTruncated = gap;
            record.expires = now + Budget::CaptureLifetime.count();
            record.created = now;
            observation->captures().put(captureId, std::move(record));
        }
        data.insert("captureId", captureId);
        return fit(data, lines, compact, maxBytesOf(job.typed), maxLinesOf(job.typed),
            captureId, job.client, entry, false);
    }
    QJsonObject fit(QJsonObject data, const QJsonArray& all, bool compact,
                   std::optional<int> maxBytes, std::optional<int> maxLinesOption,
                   const QString& id, const QString& client, const SessionDirectory::Entry& entry,
                   bool expansion, int offset = 0, int byteOffset = 0) {
        int budget = maxBytes.value_or(compact
            ? Budget::CompactPayloadBytes : Budget::ObservationDefaultPayloadBytes);
        const int maxLines = maxLinesOption.value_or(compact
            ? Budget::CompactLines : Budget::ObservationDefaultLines);
        const int end = int(all.size());
        int start = expansion ? offset
            : compact ? (std::max)(0, end - maxLines) : 0;
        QJsonArray selected;
        QList<QPair<int, int>> starts;
        int position = start;
        int nextByte = expansion ? byteOffset : 0;
        bool clippedText = false;
        for (; position < end && selected.size() < maxLines;) {
            auto line = all[position].toObject(); const auto raw = line.value("text").toString().toUtf8();
            int from = expansion ? nextByte : 0;
            if (raw.size() - from > budget && !selected.isEmpty()) break;
            if (compact && !expansion && raw.size() > budget) {
                from = int(raw.size()) - budget;
                while (from < raw.size() && (static_cast<unsigned char>(raw[from]) & 0xc0) == 0x80) ++from;
            }
            const auto clipped = clipUtf8(raw.mid(from), budget);
            if (expansion && clipped.isEmpty() && from < raw.size())
                return failure("TEXT_BUDGET_TOO_SMALL", "maxBytes cannot hold the next UTF-8 scalar.");
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
                const int next = compact && !expansion ? 0 : position;
                data.insert("expandToken", signToken({{"k", "z"}, {"c", client}, {"s", entry.id}, {"e", entry.epoch},
                    {"g", number(access->version(client))}, {"id", id}, {"offset", next},
                    {"byte", expansion ? nextByte : 0}}, key));
            } else data.insert("expandToken", QJsonValue(QJsonValue::Null));
        };
        install();
        const auto encodedSize = [&] {
            return ProtocolProjectionSizer::encodedToolResultBytes(data);
        };
        while (compact && encodedSize() > Budget::CompactFinalPayloadBytes
        && !selected.isEmpty()) {
            if (expansion) {
                if (selected.size() > 1) {
                    position = starts.last().first; nextByte = starts.last().second;
                    starts.removeLast(); selected.removeLast();
                } else {
                    const auto [lineIndex, from] = starts.constLast();
                    const auto original = all[lineIndex].toObject();
                    const auto raw = original.value("text").toString().toUtf8();
                    const int current = int(selected.at(selected.size() - 1).toObject()
                        .value("text").toString().toUtf8().size());
                    QByteArray best;
                    int low = 1;
                    int high = current;
                    while (low <= high) {
                        const int middle = low + (high - low) / 2;
                        const auto clipped = clipUtf8(raw.mid(from), middle);
                        if (clipped.isEmpty()) { low = middle + 1; continue; }
                        auto line = original;
                        line.insert("text", QString::fromUtf8(clipped));
                        if (from || from + clipped.size() != raw.size()) line.insert("partial", true);
                        selected[0] = line;
                        if (from + clipped.size() < raw.size()) {
                            position = lineIndex;
                            nextByte = from + int(clipped.size());
                        } else {
                            position = lineIndex + 1;
                            nextByte = 0;
                        }
                        install();
                        if (!clipped.isEmpty() && encodedSize() <= Budget::CompactFinalPayloadBytes) {
                            best = clipped;
                            low = middle + 1;
                        } else {
                            high = middle - 1;
                        }
                    }
                    if (best.isEmpty())
                        return failure("RESPONSE_METADATA_LIMIT", "Observation metadata leaves no room for text.");
                    auto line = original;
                    line.insert("text", QString::fromUtf8(best));
                    if (from || from + best.size() != raw.size()) line.insert("partial", true);
                    selected[0] = line;
                    if (from + best.size() < raw.size()) {
                        position = lineIndex;
                        nextByte = from + int(best.size());
                    } else {
                        position = lineIndex + 1;
                        nextByte = 0;
                    }
                }
            } else { selected.removeFirst(); starts.removeFirst(); ++start; }
            install();
        }
        if (compact && encodedSize() > Budget::CompactFinalPayloadBytes)
            return failure("RESPONSE_METADATA_LIMIT", "Observation metadata exceeds the compact budget.");
        if (!compact) {
            const bool truncated = data.value("outputTruncated").toBool();
            data.insert("resetRequired", data.value("resetRequired").toBool() || truncated);
            if (data.contains("nextToken") && (truncated || data.value("sourceTruncated").toBool()))
                data.insert("nextToken", QJsonValue(QJsonValue::Null));
        }
        return success(data);
    }
    void expand(const QString& client, const SessionDirectory::Entry& entry,
                const ReadContextRequest& request, Completion done) {
        const auto value = token(request.expandToken.value_or(QString()), client, entry, "z");
        const auto captured = value
            ? observation->captures().find(value->value("id").toString()) : std::optional<CaptureRecord>{};
        if (!value || !captured || captured->scope != CaptureScope::Client
            || captured->owner != client || captured->session != entry.id
            || captured->epoch != entry.epoch
            || captured->grantVersion != access->version(client)
            || captured->expires <= clock.elapsed()) {
            done(failure("CAPTURE_EXPIRED", "The bounded capture is no longer available.")); return;
        }
        QJsonObject data{{"sessionId", entry.id}, {"epoch", entry.epoch},
            {"captureId", value->value("id")}, {"wakeReason", "initial"},
            {"sourceGap", captured->sourceTruncated},
            {"sourceTruncated", captured->sourceTruncated},
            {"resetRequired", captured->sourceTruncated}};
        QJsonArray projected;
        for (const auto& line : captured->lines) {
            QJsonObject object{{"text", line.text}, {"source", line.source}};
            if (line.source == QLatin1String("viewport"))
                object.insert("row", line.row);
            projected.append(object);
        }
        done(fit(data, projected,
            request.projection.value_or(ProjectionMode::Compact) == ProjectionMode::Compact,
            request.maxBytes, request.maxLines, value->value("id").toString(), client, entry,
            true, value->value("offset").toInt(), value->value("byte").toInt()));
    }
    void search(const QString& client, const SessionDirectory::Entry& entry,
                const SearchContextRequest& request, Completion done) {
        const auto captured = observation->captures().find(request.capture.str());
        if (!captured || captured->scope != CaptureScope::Client || captured->owner != client
            || captured->session != entry.id || captured->epoch != entry.epoch
            || captured->grantVersion != access->version(client)
            || captured->expires <= clock.elapsed()) {
            done(failure("CAPTURE_NOT_AVAILABLE", "Capture is not available.")); return;
        }
        const QString scope = request.scope == SearchScope::Viewport ? QStringLiteral("viewport")
            : request.scope == SearchScope::RecentOutput ? QStringLiteral("recent_output")
            : QStringLiteral("both");
        const auto found = observation->search(*captured, request.query,
            request.caseSensitive, scope, request.maxMatches);
        QJsonArray matches;
        for (const auto& match : found.matches) {
            matches.append(QJsonObject{{"source", match.source},
                {"lineIndex", match.lineIndex}, {"startByte", match.startByte},
                {"endByte", match.endByte}});
        }
        done(success({{"captureId", request.capture.str()},
            {"revision", captured->revision}, {"matches", matches},
            {"limited", found.limited}, {"sourceTruncated", captured->sourceTruncated},
            {"outputTruncated", captured->outputTruncated}}));
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
        if (client.isEmpty() && session.isEmpty()) observation->captures().clear();
        else if (session.isEmpty()) observation->captures().removeClient(client);
        else if (client.isEmpty()) observation->captures().removeSession(session);
        else observation->captures().removeClientSession(client, session);
        for (auto it = replays.begin(); it != replays.end();) {
            if ((client.isEmpty() || it.key().startsWith(client + '/')) && (session.isEmpty() || it.key().endsWith('/' + session))) it = replays.erase(it); else ++it;
        }
        for (auto it = deliveredHints.begin(); it != deliveredHints.end();) {
            if ((client.isEmpty() || it.key().startsWith(client + '/')) && (session.isEmpty() || it.key().endsWith('/' + session))) it = deliveredHints.erase(it); else ++it;
        }
        if (!session.isEmpty()) {
            lastModel.remove(session); lastOutput.remove(session); observation->removeSession(session);
        }
    }
};

TerminalInteraction::TerminalInteraction(AccessStore* access, SessionDirectory* sessions,
    ObservationService* observation, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, access, sessions, observation)) {}
TerminalInteraction::~TerminalInteraction() = default;
void TerminalInteraction::call(const QString& c, const QString& r,
    const ToolRequest& request, Completion done, const QByteArray& legacyCursorKey) {
    _impl->call(c, r, request, std::move(done), legacyCursorKey);
}
void TerminalInteraction::cancel(const QString& request) { _impl->cancel(request); }
void TerminalInteraction::revoke(const QString& client, const QString& session) { _impl->revoke(client,session); }
QJsonObject TerminalInteraction::capabilities(const QString& client,
    const QString& session, bool humanConfirmation)
{
    return _impl->capabilities(client, session, humanConfirmation);
}
bool TerminalInteraction::ownsCapture(const QString& client, const QString& capture) const {
    return _impl->observation->captures().ownedByClient(capture, client);
}
bool TerminalInteraction::ownsCursor(const QString& client, const QString& cursor) const {
    const auto value = verifyToken(cursor, _impl->key);
    return value && value->value("k") == "r" && value->value("c") == client;
}
}
