#include "TerminalInteraction.h"
#include "capabilities/terminal/InputCapabilities.h"
#include "capabilities/terminal/ObservationCapabilities.h"
#include "capabilities/terminal/SearchCapabilities.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "CapabilityAdapter.h"
#include "ObservationService.h"
#include "mcp/contracts/Budget.h"
#include "mcp/application/ObservationState.h"
#include "protocol/ProtocolProjectionSizer.h"
#include "protocol/ToolCodec.h"
#include "session/SessionTerminalController.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QDateTime>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <algorithm>

namespace NovaTerm::Mcp {
namespace {
QString number(quint64 value) { return QString::number(value); }
bool commandAction(const ToolRequest& request)
{
    if (const auto* value = std::get_if<InteractTerminalRequest>(&request))
        return !value->actions.empty()
            && std::holds_alternative<CommandAction>(value->actions.front());
    return false;
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

class TerminalInteraction::Impl final : public Capabilities::Terminal::TerminalInputPort {
public:
    using Pending = ObservationWait;
    Impl(TerminalInteraction* owner, AccessStore* store, SessionDirectory* directory,
         ObservationService* observationService, Capabilities::CapabilityDispatcher& dispatcher)
        : q(owner), access(store), sessions(directory), observation(observationService),
          key(randomBytes()), state([this] { return clock.elapsed(); }), dispatcher(dispatcher), observationPorts(dispatcher, state) {
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
    ObservationState state;
    Capabilities::CapabilityDispatcher& dispatcher;
    Capabilities::Terminal::ObservationCapabilities observationPorts;
    QTimer poll;
    QTimer wake;
    QSet<TerminalSession*> watched;
    bool flushing{false};

    void watch(TerminalSession* session) {
        if (watched.contains(session)) return;
        watched.insert(session);
        QObject::connect(session, &TerminalSession::activityChanged, q, [this, session] {
            state.outputChanged(SessionId{session->id().toString(QUuid::WithoutBraces)},
                session->core()->modelRevision());
            scheduleFlush();
        });
        QObject::connect(session, &TerminalSession::terminalControlChanged, q, [this] {
            if (q->onChanged) q->onChanged();
            scheduleFlush();
        });
        QObject::connect(session, &QObject::destroyed, q, [this, session] { watched.remove(session); });
    }
    void scheduleFlush() {
        if (state.hasWaiters() && !wake.isActive()) wake.start(0);
    }
    void updatePolling() {
        if (!state.hasWaiters()) { poll.stop(); wake.stop(); }
        else if (!poll.isActive()) poll.start();
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
    std::optional<QJsonObject> token(const QString& encoded, const char* kind) const {
        const auto decoded = verifyToken(encoded, key);
        return decoded && decoded->value("k") == kind ? decoded : std::nullopt;
    }
    ObservationBinding binding(const std::optional<QJsonObject>& value) const {
        if (!value) return {};
        bool valid = false;
        const auto grant = value->value("g").toString().toULongLong(&valid);
        return {valid, ClientId{value->value("c").toString()},
            {SessionId{value->value("s").toString()}, Epoch{value->value("e").toString()}},
            ClientAccessVersion{grant}};
    }
    TerminalControlFacts controlFacts(const QString& client, const SessionDirectory::Entry& entry,
                                     bool admission = false) const {
        auto* ctrl = entry.session->terminalController();
        TerminalControlFacts facts;
        facts.session = {SessionId{entry.id}, Epoch{entry.epoch}};
        facts.grant = ClientAccessVersion{access->version(client)};
        facts.user = ctrl->userGeneration(); facts.control = ctrl->controlGeneration();
        facts.prompt = ctrl->promptGeneration(); facts.execution = ExecutionId{ctrl->executionId()};
        facts.owner = ClientId{ctrl->owner()}; facts.commandActive = ctrl->commandActive();
        facts.readyForCommand = ctrl->readyForCommand(); facts.integrated = ctrl->integrated();
        facts.pending = ctrl->pending();
        if (admission) {
            const auto capability = sessionCapabilities(entry, client, *access, false);
            if (!capability.terminalInputDisabledReason.isEmpty())
                facts.unavailable = errorCodeFromName(capability.terminalInputDisabledReason)
                    .value_or(ErrorCode::ControlInvalid);
            for (const auto& row : sessions->entries())
                if (row.session && !row.session->terminalController()->owner().isEmpty()) ++facts.controlledSessions;
        }
        return facts;
    }
    InputClaims inputClaims(const QString& encoded) const {
        const auto value = token(encoded, "i");
        InputClaims claims;
        claims.binding = binding(value);
        if (!value) return claims;
        bool valid = true;
        const auto count = [&](const char* name) {
            bool ok = false;
            const auto result = value->value(QLatin1String(name)).toString().toULongLong(&ok);
            valid &= ok;
            return result;
        };
        claims.user = count("u"); claims.control = count("t"); claims.prompt = count("p");
        claims.sequence = count("n"); claims.execution = ExecutionId{value->value("x").toString()};
        claims.expires = qint64(value->value("expires").toDouble());
        claims.binding.verified &= valid && value->value("expires").isDouble();
        return claims;
    }
    QJsonObject inputState(const QString& client, const SessionDirectory::Entry& entry) const {
        const auto claims = state.inputClaims(ClientId{client}, controlFacts(client, entry));
        return {{"k", "i"}, {"c", client}, {"s", entry.id}, {"e", entry.epoch},
            {"g", number(claims.binding.grant.value())}, {"u", number(claims.user)},
            {"t", number(claims.control)}, {"p", number(claims.prompt)},
            {"x", claims.execution.str()}, {"n", number(claims.sequence)}, {"expires", double(claims.expires)}};
    }
    ObservationCursor readCursor(const QString& encoded, const QString& client,
                                 const QByteArray& legacyKey) const {
        ObservationCursor cursor;
        if (const auto value = token(encoded, "r")) {
            cursor.binding = binding(value);
            bool modelValid = false, eventValid = false;
            cursor.model = value->value("r").toString().toULongLong(&modelValid);
            cursor.event = value->value("v").toString().toULongLong(&eventValid);
            cursor.binding.verified &= modelValid && eventValid;
        } else if (const auto legacy = legacyKey.isEmpty() ? std::nullopt : verifyToken(encoded, legacyKey)) {
            bool modelValid = false, grantValid = false;
            cursor.model = legacy->value("revision").toString().toULongLong(&modelValid);
            const auto grant = legacy->value("grant").toString().toULongLong(&grantValid);
            cursor.binding = {modelValid && grantValid && legacy->value("kind") == "context", ClientId{client},
                {SessionId{legacy->value("session").toString()}, Epoch{legacy->value("epoch").toString()}},
                ClientAccessVersion{grant}};
            cursor.legacy = true; cursor.includeViewport = legacy->value("viewport").toBool();
        }
        return cursor;
    }
    void call(const QString& client, const QString& request, const ToolRequest& typed,
              Completion done, const QByteArray& legacyCursorKey, const Capabilities::InvocationContext& context) {
        const auto reference = sessionReference(typed);
        if (!reference) { done(failure("SESSION_NOT_AVAILABLE", "Session is not shared.")); return; }
        const auto entry = sessions->find(reference->session.str());
        if (!entry || !entry->session || !access->canRead(client, *entry)) { done(failure("SESSION_NOT_AVAILABLE", "Session is not shared.")); return; }
        if (entry->epoch != reference->epoch.str()) { done(failure("STALE_SESSION_EPOCH", "Refresh the session identity.")); return; }
        watch(entry->session);
        auto* ctrl = entry->session->terminalController();
        if (const auto* searchRequest = std::get_if<SearchContextRequest>(&typed)) {
            search(client, *entry, *searchRequest, std::move(done), context); return;
        }
        const auto* readRequest = std::get_if<ReadContextRequest>(&typed);
        if (readRequest && readRequest->expandToken) {
            expand(client, *entry, *readRequest, std::move(done)); return;
        }
        if (!state.canAdmit(ClientId{client}, SessionId{entry->id})) {
            done(failure("BUSY", "Observation waiter budget is full.")); return;
        }
        Pending job;
        job.client = ClientId{client}; job.request = RequestId{request}; job.typed = typed;
        job.session = *reference; job.grant = ClientAccessVersion{access->version(client)};
        job.interaction = !readRequest;
        const auto snapshot = baseSnapshot(*entry);
        if (!snapshot) { done(failure("BUSY", "Snapshot is being prepared.", true, 50)); return; }
        job.since = snapshot->state.revision; job.event = ctrl->revision();
        job.user = ctrl->userGeneration(); job.control = ctrl->controlGeneration();
        if (readRequest) {
            const auto encoded = readRequest->sinceToken.value_or(QString());
            job.initial = encoded.isEmpty();
            if (!job.initial) {
                const auto restored = state.restoreCursor(readCursor(encoded, client, legacyCursorKey),
                    job.client, job.session, job.grant, readRequest->includeViewport.value_or(true));
                if (const auto* error = errorIfAny(restored)) { done(ToolCodec::encodeFailure(*error)); return; }
                const auto& cursor = std::get<ObservationCursor>(restored);
                job.since = cursor.model;
                if (!cursor.legacy) job.event = cursor.event;
            }
        } else {
            const auto facts = controlFacts(client, *entry, true);
            if (facts.unavailable) { done(ToolCodec::encodeFailure(Error{*facts.unavailable})); return; }
            const Capabilities::Terminal::InputCapabilities ports{dispatcher, state, *this};
            auto inputContext = context;
            inputContext.operation = Capabilities::OperationId{context.operation.value() + "/input"};
            if (const auto* elevation = std::get_if<RequestElevationRequest>(&typed)) {
                QJsonObject response;
                ports.elevation.invoke({*elevation, inputClaims(elevation->inputToken)}, inputContext,
                    [&response](auto value) {
                        if (const auto* result = std::get_if<Result<std::monostate>>(&value)) {
                            if (const auto* error = errorIfAny(*result)) response = ToolCodec::encodeFailure(*error);
                        } else response = ToolCodec::encodeFailure(Error{ErrorCode::SessionNotAvailable});
                    });
                done(std::move(response)); return;
            }
            const auto& input = std::get<InteractTerminalRequest>(typed);
            Result<InputAdmission> prepared{Error{ErrorCode::SessionNotAvailable}};
            ports.input.invoke({input, inputClaims(input.inputToken)}, inputContext, [&prepared](auto value) {
                if (const auto* result = std::get_if<Result<InputAdmission>>(&value)) prepared = *result;
            });
            if (const auto* error = errorIfAny(prepared)) { done(ToolCodec::encodeFailure(*error)); return; }
            if (std::get<InputAdmission>(prepared) == InputAdmission::Replay) job.immediate = ObservationWake::StateChanged;
            else { job.event = ctrl->revision(); job.control = ctrl->controlGeneration(); }

        }
        auto waitContext = context;
        waitContext.operation = Capabilities::OperationId{context.operation.value() + "/observe"};
        auto& port = readRequest ? observationPorts.read : observationPorts.input;
        const QPointer<TerminalInteraction> guard(q);
        port.invoke(job, waitContext, [this, guard, done = std::move(done)](auto result) {
            if (!guard) return;
            const auto* completed = std::get_if<ObservationCompletion>(&result);
            if (!completed) { done(ToolCodec::encodeFailure(Error{ErrorCode::SessionNotAvailable})); return; }
            if (completed->error) { done(ToolCodec::encodeFailure(*completed->error)); return; }
            const auto current = sessions->find(completed->wait.session.session.str());
            const auto base = current && current->session ? baseSnapshot(*current) : nullptr;
            if (!current || !base) done(failure("SESSION_NOT_AVAILABLE", "The observation connection changed."));
            else done(project(completed->wait, *current, *base, wakeName(completed->wake)));
        });
        ctrl->touch(client);
        updatePolling(); scheduleFlush();
    }

    TerminalControlFacts facts(const ClientId& client, const SessionReference& reference) override {
        const auto entry = sessions->find(reference.session.str());
        if (!entry || entry->epoch != reference.epoch.str() || !access->canRead(client.str(), *entry)) {
            TerminalControlFacts result; result.unavailable = ErrorCode::SessionNotAvailable; return result;
        }
        return controlFacts(client.str(), *entry, true);
    }
    bool acquire(const ClientId& client, const SessionReference& reference) override {
        const auto entry = sessions->find(reference.session.str());
        return entry && entry->session && entry->epoch == reference.epoch.str()
            && entry->session->terminalController()->acquire(client.str());
    }
    bool submit(const ClientId& client, const InteractTerminalRequest& request) override {
        const auto entry = sessions->find(request.session.str());
        if (!entry || !entry->session || entry->epoch != request.epoch.str()
            || !access->canControlTerminal(client.str(), *entry)) return false;
        auto* controller = entry->session->terminalController();
        return controller->submit(client.str(), request.operation.str(), encode(request.actions, controller->lineEnding()),
            commandAction(ToolRequest{request}), int(request.actions.size()));
    }
    void release(const ClientId& client, const InteractTerminalRequest& request) override {
        const auto entry = sessions->find(request.session.str());
        if (entry && entry->session && entry->epoch == request.epoch.str())
            entry->session->terminalController()->release(client.str(), QStringLiteral("released"), request.operation.str());
    }
    bool canElevate(const ClientId& client, const SessionReference& reference) override {
        const auto entry = sessions->find(reference.session.str());
        return entry && entry->epoch == reference.epoch.str() && access->canUseElevation(client.str(), *entry);
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

    static QString wakeName(ObservationWake wake) {
        switch (wake) {
        case ObservationWake::Initial: return QStringLiteral("initial");
        case ObservationWake::Deadline: return QStringLiteral("deadline");
        case ObservationWake::StateChanged: return QStringLiteral("state_changed");
        case ObservationWake::ControlChanged: return QStringLiteral("control_changed");
        case ObservationWake::CommandFinished: return QStringLiteral("command_finished");
        case ObservationWake::OutputChanged: return QStringLiteral("output_changed");
        case ObservationWake::AttentionCandidate: return QStringLiteral("attention_candidate");
        case ObservationWake::DiagnosticCandidate: return QStringLiteral("diagnostic_candidate");
        }
        return {};
    }
    void deliver(const ObservationCompletion& completed) { observationPorts.complete(completed); }
    void flush() {
        if (flushing) return;
        flushing = true;
        for (const auto& job : state.waiters()) {
            const auto entry = sessions->find(job.session.session.str());
            ObservationFacts facts;
            facts.available = entry && entry->session && access->canRead(job.client.str(), *entry);
            std::shared_ptr<const TerminalContextProvider::Snapshot> snapshot;
            if (facts.available) {
                facts.session = {SessionId{entry->id}, Epoch{entry->epoch}};
                facts.grant = ClientAccessVersion{access->version(job.client.str())};
                auto* ctrl = entry->session->terminalController();
                facts.event = ctrl->revision(); facts.user = ctrl->userGeneration();
                facts.control = ctrl->controlGeneration(); facts.running = entry->state == SessionState::Running;
                facts.commandFinished = ctrl->observation(job.client.str()).value("command").toObject().value("state") == "finished";
                snapshot = baseSnapshot(*entry);
                facts.snapshotAvailable = bool(snapshot);
                if (snapshot) {
                    facts.model = snapshot->state.revision;
                    for (auto it = snapshot->state.viewport.crbegin(); it != snapshot->state.viewport.crend(); ++it) {
                        auto line = QString::fromUtf8(it->text.data(), qsizetype(it->text.size()));
                        if (!line.trimmed().isEmpty()) { facts.lastVisibleLine = std::move(line); break; }
                    }
                }
            }
            if (const auto completed = state.evaluate(job.request, facts)) {
                if (!completed->error && entry && entry->session)
                    entry->session->terminalController()->touch(job.client.str());
                deliver(*completed);
            }
        }
        flushing = false;
        updatePolling();
    }

    QJsonObject project(const Pending& job, const SessionDirectory::Entry& entry,
                        const TerminalContextProvider::Snapshot& snapshot, const QString& wake) {
        auto* ctrl = entry.session->terminalController();
        const bool compact = projectionOf(job.typed).value_or(
            job.interaction ? ProjectionMode::Compact : ProjectionMode::Full)
            == ProjectionMode::Compact;
        const bool changed = job.since != snapshot.state.revision;
        QJsonObject data = ctrl->observation(job.client.str());
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
        QJsonObject cursor{{"k", "r"}, {"c", job.client.str()}, {"s", entry.id}, {"e", entry.epoch},
            {"g", number(job.grant.value())}, {"r", number(snapshot.state.revision)}, {"v", number(ctrl->revision())}};
        data.insert("nextToken", signToken(cursor, key));
        const bool canInput = capabilities(job.client.str(), entry.id).value("availability").toObject().value("inputAvailable").toBool();
        if (canInput) data.insert("inputToken", signToken(inputState(job.client.str(), entry), key));
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
            record.owner = job.client.str();
            record.session = entry.id;
            record.epoch = entry.epoch;
            record.grantVersion = job.grant.value();
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
            captureId, job.client.str(), entry, false);
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
        const auto value = token(request.expandToken.value_or(QString()), "z");
        const auto captured = value
            ? observation->captures().find(value->value("id").toString()) : std::optional<CaptureRecord>{};
        const SessionReference reference{SessionId{entry.id}, Epoch{entry.epoch}};
        const auto grant = ClientAccessVersion{access->version(client)};
        if (!value || !captured || !state.matches(binding(value), ClientId{client}, reference, grant)
            || !state.captureAvailable(*captured, ClientId{client}, reference, grant)) {
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
                const SearchContextRequest& request, Completion done, const Capabilities::InvocationContext& context) {
        const auto captured = observation->captures().find(request.capture.str());
        if (!captured || !state.captureAvailable(*captured, ClientId{client},
            {SessionId{entry.id}, Epoch{entry.epoch}}, ClientAccessVersion{access->version(client)})) {
            done(failure("CAPTURE_NOT_AVAILABLE", "Capture is not available.")); return;
        }
        const Capabilities::Terminal::SearchCapabilities ports{dispatcher,
            [record = *captured](const auto&, const auto&) -> Result<CaptureRecord> { return record; },
            [](CaptureRecord record, SearchContextRequest query, auto deliver) {
                const auto scope = query.scope == SearchScope::Viewport ? QStringLiteral("viewport")
                    : query.scope == SearchScope::RecentOutput ? QStringLiteral("recent_output") : QStringLiteral("both");
                deliver(searchCapture(record, query.query, query.caseSensitive, scope, query.maxMatches));
            }};
        auto searchContext = context;
        searchContext.operation = Capabilities::OperationId{context.operation.value() + "/search"};
        ports.search.invoke(request, searchContext, [done = std::move(done)](auto value) {
            const auto* domain = std::get_if<Result<Capabilities::Terminal::ContextSearchResult>>(&value);
            if (!domain) { done(ToolCodec::encodeFailure(Error{ErrorCode::SessionNotAvailable})); return; }
            if (const auto* error = errorIfAny(*domain)) { done(ToolCodec::encodeFailure(*error)); return; }
            const auto& result = std::get<Capabilities::Terminal::ContextSearchResult>(*domain);
            QJsonArray matches;
            for (const auto& match : result.search.matches) matches.append(QJsonObject{{"source", match.source},
                {"lineIndex", match.lineIndex}, {"startByte", match.startByte}, {"endByte", match.endByte}});
            done(success({{"captureId", result.capture.str()}, {"revision", result.revision}, {"matches", matches},
                {"limited", result.search.limited}, {"sourceTruncated", result.sourceTruncated}, {"outputTruncated", result.outputTruncated}}));
        });
    }
    void cancel(const QString& request) {
        observationPorts.cancel(RequestId{request});
        updatePolling();
    }
    void revoke(const QString& client, const QString& session) {
        // Remove application state before releasing Session control or invoking a callback.
        const auto revoked = state.revoke(ClientId{client}, SessionId{session});
        for (const auto& entry : sessions->entries()) if ((session.isEmpty() || session == entry.id) && entry.session) {
            auto* ctrl = entry.session->terminalController();
            if (client.isEmpty() || ctrl->owner() == client) ctrl->release(client, "revoked");
        }
        if (client.isEmpty() && session.isEmpty()) observation->captures().clear();
        else if (session.isEmpty()) observation->captures().removeClient(client);
        else if (client.isEmpty()) observation->captures().removeSession(session);
        else observation->captures().removeClientSession(client, session);
        if (!session.isEmpty()) observation->removeSession(session);
        for (const auto& completed : revoked) deliver(completed);
        updatePolling();
    }
};

TerminalInteraction::TerminalInteraction(AccessStore* access, SessionDirectory* sessions,
    ObservationService* observation, Capabilities::CapabilityDispatcher& dispatcher, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, access, sessions, observation, dispatcher)) {}
TerminalInteraction::~TerminalInteraction() = default;
void TerminalInteraction::call(const QString& c, const QString& r,
    const ToolRequest& request, Completion done, const QByteArray& legacyCursorKey, const Capabilities::InvocationContext& context) {
    _impl->call(c, r, request, std::move(done), legacyCursorKey, context);
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
