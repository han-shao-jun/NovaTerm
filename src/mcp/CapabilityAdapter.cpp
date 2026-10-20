/** @file CapabilityAdapter.cpp @brief Session 事实到能力模型的唯一转换实现。 */
#include "CapabilityAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "ObservationService.h"
#include "CommandPolicy.h"

#include "session/ISessionScriptProvider.h"
#include "session/SessionCommandCoordinator.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionInputArbiter.h"
#include "session/SessionTerminalController.h"

#include <QJsonArray>

namespace NovaTerm::Mcp {

CapabilityFacts capabilityFacts(const SessionDirectory::Entry& entry,
                                const AccessStore& access)
{
    CapabilityFacts facts;
    facts.sessionRunning = entry.state == SessionState::Running
        && entry.session && entry.session->state() == SessionState::Running;
    facts.transportConnected = entry.transport && entry.transport->isConnected();
    facts.transportMatchesSession = entry.session
        && entry.session->transport() == entry.transport;
    facts.generationMatches = entry.session
        && entry.session->statistics().generation == entry.generation;
    facts.targetFingerprintPresent = !entry.targetFingerprint.isEmpty();

    auto* facade = entry.session ? entry.session->commandFacade() : nullptr;
    const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
    facts.diagnosticsExecutorAvailable = facade && facade->isAvailable()
        && profile.isAvailable();
    for (const auto& id : profile.supportedCommandIds())
        facts.supportedDiagnostics.insert(DiagnosticId{id});

    auto* coordinator = entry.session ? entry.session->commandCoordinator() : nullptr;
    facts.unverifiedPrompt = entry.kind == TransportKind::Ssh
        && coordinator && coordinator->allowsUnverifiedPrompt();
    facts.idleCompletion = entry.kind == TransportKind::Serial
        && coordinator && coordinator->allowsIdleCompletion();
    const bool trustedLocal = entry.kind == TransportKind::LocalShell
        && coordinator && coordinator->hasTrustedProfile()
        && coordinator->isPromptReady();
    facts.interactiveReachable = facade && facade->isAvailable()
        && !entry.targetFingerprint.isEmpty()
        && (facade->capabilities().mode == CommandExecutionMode::InteractiveFramed
            || facts.unverifiedPrompt || facts.idleCompletion || trustedLocal);
    facts.scriptProviderAvailable = entry.session && entry.session->scriptProvider()
        && entry.session->scriptProvider()->isAvailable();

    facts.transferLease = entry.session
        && entry.session->inputArbiter()->hasTransferLease();
    facts.legacyTerminalBusy = entry.session
        && entry.session->inputArbiter()->hasMcpLease()
        && entry.session->terminalController()->owner().isEmpty();
    facts.terminalOwner = entry.session
        ? entry.session->terminalController()->owner() : QString{};
    facts.shellIntegrationAvailable = entry.session
        && entry.session->terminalController()->integrated();
    facts.elevationTargetVerified = access.elevationTargetIdentityVerified(entry);
    return facts;
}

CapabilitySnapshot sessionCapabilities(const SessionDirectory::Entry& entry,
    const QString& clientId, const AccessStore& access, bool humanConfirmation)
{
    CapabilityContext context;
    context.client = ClientId{clientId};
    context.humanConfirmation = humanConfirmation;
    context.credentialUpdatePending = access.credentialUpdatePending();
    return CapabilityResolver::resolve(
        access.grantSnapshot(context.client, entry),
        capabilityFacts(entry, access), context);
}

namespace {
ReadSession readSession(const SessionDirectory::Entry& entry)
{
    ReadSession row;
    row.reference = {SessionId{entry.id}, Epoch{entry.epoch}};
    row.state = SessionDirectory::stateName(entry.state);
    row.transport = SessionDirectory::transportName(entry.kind);
    row.displayName = boundedText(entry.title, 256, &row.displayNameTruncated);
    row.running = entry.state == SessionState::Running;
    row.visible = entry.state != SessionState::Closing && entry.state != SessionState::Closed;
    return row;
}

QJsonObject capabilityMetadata(const CapabilitySnapshot& capability)
{
    QJsonArray names;
    for (const auto& name : capability.capabilities)
        names.append(name);
    return {{"capabilities", names},
        {"permissions", QJsonObject{{"readOutput", capability.permissions.read},
            {"terminalInput", capability.terminalInputAvailable},
            {"useElevation", capability.terminalInputAvailable && capability.permissions.elevation}}},
        {"availability", QJsonObject{{"inputAvailable", capability.terminalInputAvailable},
            {"disabledReason", capability.terminalInputDisabledReason},
            {"elevationAvailable", capability.elevationAvailable},
            {"elevationDisabledReason", capability.elevationDisabledReason},
            {"ownedByThisClient", capability.ownedByThisClient},
            {"controlState", capability.terminalOwner.isEmpty() ? "idle" : "controlled"}}},
        {"shellIntegration", QJsonObject{{"available", capability.shellIntegrationAvailable}}},
        {"elevationMethods", QJsonArray{}}};
}

Result<DirectoryCursor> directoryCursor(const QString& encoded, const QByteArray& key)
{
    const auto token = verifyToken(encoded, key);
    bool revisionValid = false, grantValid = false;
    const auto revision = token ? token->value("version").toString().toULongLong(&revisionValid) : 0;
    const auto grant = token ? token->value("grant").toString().toULongLong(&grantValid) : 0;
    if (!token || token->value("kind") != "directory" || !revisionValid || !grantValid)
        return Error{ErrorCode::CursorStale};
    return DirectoryCursor{SessionId{token->value("after").toString()}, revision, ClientAccessVersion{grant}};
}

Result<ContextCursor> contextCursor(const QString& encoded, const QByteArray& key)
{
    const auto token = verifyToken(encoded, key);
    bool revisionValid = false, grantValid = false;
    const auto revision = token ? token->value("revision").toString().toULongLong(&revisionValid) : 0;
    const auto grant = token ? token->value("grant").toString().toULongLong(&grantValid) : 0;
    if (!token || token->value("kind") != "context" || !revisionValid || !grantValid)
        return Error{ErrorCode::CursorStale};
    return ContextCursor{{SessionId{token->value("session").toString()}, Epoch{token->value("epoch").toString()}},
        revision, ClientAccessVersion{grant}, token->value("viewport").toBool()};
}
} // namespace

namespace {
template<class Query, class Value>
Result<Value> invokeRead(const Capabilities::TypedCapabilityPort<Query, Result<Value>>& port,
    Query query, const Capabilities::InvocationContext& context)
{
    Result<Value> result{Error{ErrorCode::SessionNotAvailable}};
    port.invoke(std::move(query), context, [&result](Capabilities::Result<Result<Value>> value) {
        if (const auto* accepted = std::get_if<Result<Value>>(&value)) result = *accepted;
        else if (std::get<Capabilities::Rejection>(value) == Capabilities::Rejection::DeadlineExceeded)
            result = Error{ErrorCode::DeadlineExceeded};
    });
    return result;
}
} // namespace

ReadUseCaseAdapter::ReadUseCaseAdapter(Ports ports, Capabilities::CapabilityDispatcher& dispatcher,
    const Capabilities::InvocationContext& context)
    : _ports(std::move(ports)), _dispatcher(dispatcher), _context(context) {}

quint64 ReadUseCaseAdapter::directoryRevision() const { return _ports.sessions->revision(); }
ClientAccessVersion ReadUseCaseAdapter::accessVersion(const ClientId& client) const
{
    return ClientAccessVersion{_ports.access->version(client.str())};
}

QList<ReadSession> ReadUseCaseAdapter::pageAfter(const ClientId& client,
    const SessionId& after, int limit, bool humanConfirmation) const
{
    const auto entries = _ports.sessions->pageAfter(after.str(), limit,
        [this, &client](const SessionDirectory::Entry& entry) {
            return _ports.access->canRead(client.str(), entry)
                && entry.state != SessionState::Closing && entry.state != SessionState::Closed;
        });
    QList<ReadSession> rows;
    for (const auto& entry : entries) {
        auto row = readSession(entry);
        row.capabilities = sessionCapabilities(entry, client.str(), *_ports.access, humanConfirmation);
        rows.append(std::move(row));
    }
    return rows;
}

Result<ContextSummary> ReadUseCaseAdapter::readContext(const ContextQuery& query) const
{
    const auto entry = _ports.sessions->find(query.session.session.str());
    if (!entry || !entry->session || entry->epoch != query.session.epoch.str()
        || !_ports.access->canRead(query.client.str(), *entry))
        return Error{ErrorCode::SessionNotAvailable};
    ObservationReadRequest request;
    request.sessionId = query.session.session.str();
    request.epoch = query.session.epoch.str();
    request.owner = query.owner;
    request.captureId = query.capture.str();
    request.includeViewport = query.includeViewport;
    request.sinceProvided = query.sinceRevision.has_value();
    request.sinceRevision = query.sinceRevision.value_or(0);
    request.maxBytes = query.maxBytes;
    request.maxLines = query.maxLines;
    request.freshness = NovaTerm::publishedContextSnapshotEnabled()
        ? FreshnessPolicy::BoundedStaleness : FreshnessPolicy::StrictRevision;
    const auto observed = _ports.observation->read(request, *entry);
    if (observed.status != ObservationRead::Status::Ok) {
        const auto delay = observed.status == ObservationRead::Status::Busy ? 250 : 50;
        return Error{ErrorCode::Busy, QStringLiteral("BUSY"), RetryHint{true, std::chrono::milliseconds(delay)}};
    }
    ContextSummary summary;
    summary.capture = CaptureId{observed.captureId};
    summary.revision = observed.revision.toULongLong();
    summary.capturedAt = observed.capturedAt;
    summary.title = observed.title;
    summary.sourceTruncated = observed.sourceTruncated;
    summary.outputTruncated = observed.outputTruncated;
    summary.resetRequired = observed.resetRequired;
    summary.alternateScreen = observed.alternateScreen;
    summary.historyMayPrecedeEpoch = observed.historyMayPrecedeEpoch;
    summary.cursorRow = observed.cursorRow;
    summary.cursorColumn = observed.cursorColumn;
    summary.suppressedDuplicates = observed.suppressedDuplicates;
    for (const auto& line : observed.viewport) summary.viewport.append(line.text);
    for (const auto& line : observed.recent) summary.recent.append(line.text);
    return summary;
}

DiagnosticCatalog ReadUseCaseAdapter::commandCatalog(const ClientId& client,
    const SessionReference& session) const
{
    const auto entry = _ports.sessions->find(session.session.str());
    DiagnosticCatalog catalog;
    if (!entry || entry->epoch != session.epoch.str())
        return catalog;
    auto* facade = entry->session ? entry->session->commandFacade() : nullptr;
    const auto profile = facade ? facade->profile() : CommandPlatformProfile{};
    catalog.policyVersion = CommandPolicy::version(profile);
    catalog.profileVersion = profile.version();
    catalog.profileAvailable = profile.isAvailable();
    catalog.executorAvailable = facade && facade->isAvailable();
    catalog.targetVerified = !entry->targetFingerprint.isEmpty();
    for (const auto& id : _ports.access->commands(client.str(), *entry))
        catalog.allowed.insert(DiagnosticId{id});
    for (const auto& command : CommandPolicy::catalog(profile)) {
        const auto preview = entry->kind == TransportKind::LocalShell && facade
            && facade->capabilities().mode == CommandExecutionMode::Isolated
            ? QStringLiteral("novaterm-local-diag ") + command.id
            : QString::fromUtf8(command.command);
        catalog.commands.append({DiagnosticId{command.id}, command.title, preview});
    }
    return catalog;
}

Result<QJsonObject> ReadUseCaseAdapter::listSessions(const QString& client,
    const QByteArray& key, bool formElicitation, const ListSessionsRequest& request) const
{
    std::optional<DirectoryCursor> cursor;
    if (request.cursor && !request.cursor->isEmpty()) {
        const auto decoded = directoryCursor(*request.cursor, key);
        if (const auto* error = errorIfAny(decoded)) return *error;
        cursor = std::get<DirectoryCursor>(decoded);
    }
    Q_UNUSED(client);
    const Capabilities::Terminal::ReadCapabilities ports{_dispatcher, *this};
    const auto result = invokeRead(ports.sessions,
        Capabilities::Terminal::SessionListQuery{request.limit, cursor, formElicitation}, _context);
    if (const auto* error = errorIfAny(result)) return *error;
    const auto& page = std::get<SessionPage>(result);
    QJsonArray rows;
    for (const auto& row : page.sessions) {
        auto encoded = capabilityMetadata(row.capabilities);
        encoded.insert("sessionId", row.reference.session.str());
        encoded.insert("epoch", row.reference.epoch.str());
        encoded.insert("state", row.state);
        encoded.insert("transport", row.transport);
        encoded.insert("displayName", row.displayName);
        encoded.insert("displayNameTruncated", row.displayNameTruncated);
        rows.append(encoded);
    }
    QJsonValue next(QJsonValue::Null);
    if (page.next) next = signToken({{"kind", "directory"}, {"after", page.next->after.str()},
        {"version", QString::number(page.next->directoryRevision)},
        {"grant", QString::number(page.next->accessVersion.value())}}, key);
    return QJsonObject{{"instanceId", _ports.instance}, {"applicationVersion", NOVATERM_VERSION},
        {"sessions", rows}, {"nextCursor", next}};
}

Result<QJsonObject> ReadUseCaseAdapter::readContext(const SessionDirectory::Entry& entry,
    const QString& owner, const QString& client, const QByteArray& key,
    const ReadContextRequest& request) const
{
    std::optional<ContextCursor> cursor;
    if (request.sinceToken && !request.sinceToken->isEmpty()) {
        const auto decoded = contextCursor(*request.sinceToken, key);
        if (const auto* error = errorIfAny(decoded)) return *error;
        cursor = std::get<ContextCursor>(decoded);
    }
    ContextQuery query;
    query.session = {SessionId{entry.id}, Epoch{entry.epoch}};
    query.owner = owner;
    query.capture = CaptureId{newId()};
    query.includeViewport = request.includeViewport.value_or(true);
    query.maxBytes = request.maxBytes.value_or(Budget::ObservationDefaultPayloadBytes);
    query.maxLines = request.maxLines.value_or(Budget::ObservationDefaultLines);
    Q_UNUSED(client);
    const Capabilities::Terminal::ReadCapabilities ports{_dispatcher, *this};
    const auto result = invokeRead(ports.context, Capabilities::Terminal::ContextReadQuery{query, cursor}, _context);
    if (const auto* error = errorIfAny(result)) return *error;
    const auto& page = std::get<ContextPage>(result);
    const auto& observed = page.summary;
    QJsonValue next(QJsonValue::Null);
    if (page.next) next = signToken({{"kind", "context"}, {"session", page.next->session.session.str()},
        {"epoch", page.next->session.epoch.str()}, {"revision", QString::number(page.next->revision)},
        {"grant", QString::number(page.next->accessVersion.value())}, {"viewport", page.next->includeViewport}}, key);
    QJsonArray screen, recent;
    for (const auto& line : observed.viewport) screen.append(QJsonObject{{"text", line}});
    for (const auto& line : observed.recent) recent.append(QJsonObject{{"text", line}});
    return QJsonObject{{"instanceId", _ports.instance}, {"sessionId", entry.id},
        {"epoch", entry.epoch}, {"revision", QString::number(observed.revision)},
        {"captureId", observed.capture.str()}, {"capturedAt", observed.capturedAt},
        {"state", SessionDirectory::stateName(entry.state)},
        {"transport", SessionDirectory::transportName(entry.kind)},
        {"title", observed.title}, {"alternateScreen", observed.alternateScreen},
        {"cursor", QJsonObject{{"row", observed.cursorRow}, {"column", observed.cursorColumn}}},
        {"viewport", screen}, {"recentOutput", recent}, {"coverage", "bounded_summary"},
        {"historyMayPrecedeEpoch", observed.historyMayPrecedeEpoch},
        {"sourceTruncated", observed.sourceTruncated}, {"outputTruncated", observed.outputTruncated},
        {"resetRequired", observed.resetRequired},
        {"suppressedDuplicates", QString::number(observed.suppressedDuplicates)}, {"nextToken", next}};
}

Result<QJsonObject> ReadUseCaseAdapter::listCommands(const SessionDirectory::Entry& entry,
    const QString& client, const QByteArray& key, qint64 now) const
{
    Q_UNUSED(client);
    const Capabilities::Terminal::ReadCapabilities ports{_dispatcher, *this};
    const auto result = invokeRead(ports.diagnostics,
        Capabilities::Terminal::DiagnosticListQuery{readSession(entry), now}, _context);
    if (const auto* error = errorIfAny(result)) return *error;
    const auto& list = std::get<CommandList>(result);
    QJsonArray catalog;
    for (const auto& command : list.commands) {
        const auto ticket = signToken({{"kind", "command"}, {"execution", newId()},
            {"session", list.session.session.str()}, {"epoch", list.session.epoch.str()},
            {"command", command.id.str()}, {"policy", list.policyVersion}, {"profile", list.profileVersion},
            {"grant", QString::number(list.accessVersion.value())}, {"expires", list.expires}}, key);
        catalog.append(QJsonObject{{"commandId", command.id.str()}, {"title", command.title},
            {"preview", command.preview},
            {"argumentSchema", QJsonObject{{"type", "object"}, {"additionalProperties", false}}},
            {"timeoutMs", int(Budget::CommandTimeout.count())}, {"maxOutputBytes", Budget::CommandOutputBytes},
            {"commandTicket", ticket}});
    }
    const QJsonValue reason = list.disabledReason
        ? QJsonValue(QString::fromLatin1(errorCodeName(*list.disabledReason))) : QJsonValue(QJsonValue::Null);
    return QJsonObject{{"instanceId", _ports.instance}, {"sessionId", entry.id}, {"epoch", entry.epoch},
        {"policyVersion", list.policyVersion}, {"executionEnabled", !list.disabledReason},
        {"disabledReason", reason}, {"commands", catalog}};
}

} // namespace NovaTerm::Mcp
