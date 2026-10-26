/** @file SettingsController.cpp @brief 界面快照装配与单项授权提交实现。 */
#include "SettingsController.h"
#include "mcp/CapabilityAdapter.h"
#include "mcp/CommandPolicy.h"
#include "mcp/McpAccess.h"
#include "mcp/McpService.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionTerminalController.h"

namespace NovaTerm::Ui {
namespace {

using namespace NovaTerm::Mcp;

bool clearTerminalSelectable(const SessionDirectory::Entry& entry,
                             const PermissionSelection& selection)
{
    // 已授予权限始终可以撤回；新授权由服务端再次核验同一就绪事实。
    return selection.clearTerminal || (selection.read
        && entry.session && entry.transport && entry.state == SessionState::Running
        && entry.session->state() == SessionState::Running
        && entry.transport->isConnected() && entry.session->transport() == entry.transport
        && entry.session->statistics().generation == entry.generation
        && !entry.epoch.isEmpty() && !entry.attachmentId.isEmpty());
}

QString rejectionName(AuthorizationRejection reason)
{
    switch (reason) {
    case AuthorizationRejection::Disabled: return QStringLiteral("MCP_DISABLED");
    case AuthorizationRejection::UnknownClient: return QStringLiteral("UNKNOWN_CLIENT");
    case AuthorizationRejection::StaleBinding: return QStringLiteral("STALE_BINDING");
    case AuthorizationRejection::StaleRevision: return QStringLiteral("STALE_REVISION");
    case AuthorizationRejection::InvalidSelection: return QStringLiteral("INVALID_SELECTION");
    case AuthorizationRejection::SessionNotReady: return QStringLiteral("SESSION_NOT_READY");
    case AuthorizationRejection::CredentialsPending:
        return QStringLiteral("CREDENTIAL_UPDATE_IN_PROGRESS");
    case AuthorizationRejection::None: break;
    }
    return {};
}

} // namespace

SettingsController::SettingsController(Service* service) : _service(service) {}

QVector<SessionPermissionRow> SettingsController::rows(const QString& clientId) const
{
    QVector<SessionPermissionRow> result;
    if (!_service)
        return result;
    auto& access = _service->access();
    const auto catalog = CommandPolicy::catalog();
    for (const auto& entry : _service->directory().entries()) {
        SessionPermissionRow row;
        row.client = ClientId{clientId};
        row.key = SessionRowKey{entry.id, entry.attachmentId, entry.epoch,
            entry.generation, entry.targetFingerprint};
        row.title = entry.title.isEmpty() ? entry.id : entry.title;
        row.state = SessionDirectory::stateName(entry.state);
        const auto grant = access.grantSnapshot(row.client, entry);
        row.selection = grant.selection;
        row.revision = grant.revision;
        row.binding = grant.binding;
        row.capability = sessionCapabilities(entry, clientId, access, false);
        row.localShell = entry.kind == TransportKind::LocalShell;
        if (entry.session) {
            auto* terminal = entry.session->terminalController();
            row.terminalOwner = terminal->owner();
            const auto current = terminal->observation(clientId);
            row.controlState = current.value("control").toObject()
                .value("state").toString();
            row.commandState = current.value("command").toObject()
                .value("state").toString();
        }
        row.diagnostics.reserve(catalog.size());
        const bool isolatedLocal = entry.kind == TransportKind::LocalShell
            && entry.session
            && entry.session->commandFacade()->capabilities().mode
                == CommandExecutionMode::Isolated;
        for (const auto& command : catalog) {
            row.diagnostics.append(DiagnosticRow{command.id, command.title,
                isolatedLocal ? QStringLiteral("novaterm-local-diag ") + command.id
                              : QString::fromUtf8(command.command),
                grant.selection.diagnostics.contains(DiagnosticId{command.id})});
        }
        row.elevationSelectable = row.capability.elevationGrantSelectable;
        row.clearTerminalSelectable = clearTerminalSelectable(entry, row.selection);
        row.sudoSaved = access.hasElevationCredential(entry, QStringLiteral("sudo"));
        row.suSaved = access.hasElevationCredential(entry, QStringLiteral("su"));
        row.credentialsPersistent = access.elevationCredentialsPersistent();
        result.append(std::move(row));
    }
    return result;
}

SettingsController::Outcome SettingsController::submit(const SessionPermissionRow& row,
    NovaTerm::Mcp::PermissionKind kind,
    std::optional<NovaTerm::Mcp::DiagnosticId> diagnostic, bool selected) const
{
    Outcome outcome;
    if (!_service)
        return outcome;
    const auto entry = _service->directory().find(row.key.sessionId);
    if (!entry) {
        outcome.status = Status::Stale;
        outcome.reason = QStringLiteral("SESSION_NOT_AVAILABLE");
        return outcome;
    }
    const SessionRowKey current{entry->id, entry->attachmentId, entry->epoch,
        entry->generation, entry->targetFingerprint};
    if (!(current == row.key)) {
        outcome.status = Status::Stale;
        outcome.reason = QStringLiteral("STALE_BINDING");
        return outcome;
    }
    PermissionChange change;
    change.client = row.client;
    change.binding = row.binding;
    change.expectedRevision = row.revision;
    change.kind = kind;
    change.diagnostic = diagnostic;
    change.selected = selected;
    auto result = _service->access().changePermission(change, *entry);
    if (isError(result)) {
        const auto reason = errorIfAny(result)->reason;
        outcome.status = reason == AuthorizationRejection::StaleBinding
                || reason == AuthorizationRejection::StaleRevision
            ? Status::Stale : Status::Rejected;
        outcome.reason = rejectionName(reason);
        return outcome;
    }
    const auto& grant = std::get<GrantSnapshot>(result);
    outcome.status = Status::Applied;
    outcome.row = row;
    outcome.row.selection = grant.selection;
    outcome.row.revision = grant.revision;
    outcome.row.binding = grant.binding;
    outcome.row.clearTerminalSelectable = clearTerminalSelectable(*entry, grant.selection);
    outcome.row.capability = sessionCapabilities(
        *entry, row.client.str(), _service->access(), false);
    for (auto& diagnosticRow : outcome.row.diagnostics)
        diagnosticRow.granted = grant.selection.diagnostics.contains(
            DiagnosticId{diagnosticRow.id});
    return outcome;
}

bool SettingsController::enabled() const
{
    return _service && _service->access().enabled();
}

bool SettingsController::setEnabled(bool enabled)
{
    return _service && _service->access().setEnabled(enabled);
}

QList<NovaTerm::Mcp::ClientRecord> SettingsController::clients() const
{
    return _service ? _service->access().clients()
                    : QList<NovaTerm::Mcp::ClientRecord>{};
}

QString SettingsController::addClient(const QString& label)
{
    return _service ? _service->access().addClient(label) : QString{};
}

bool SettingsController::removeClient(const QString& id)
{
    return _service && _service->access().removeClient(id);
}

bool SettingsController::rotateToken(const QString& id)
{
    return _service && _service->access().rotateToken(id);
}

SettingsController::SaveFailure SettingsController::lastSaveFailure() const
{
    if (!_service)
        return SaveFailure::None;
    switch (_service->access().lastSaveFailure()) {
    case NovaTerm::Mcp::AccessStore::SaveFailure::Directory: return SaveFailure::Directory;
    case NovaTerm::Mcp::AccessStore::SaveFailure::Locked: return SaveFailure::Locked;
    case NovaTerm::Mcp::AccessStore::SaveFailure::Revision: return SaveFailure::Revision;
    case NovaTerm::Mcp::AccessStore::SaveFailure::Write: return SaveFailure::Write;
    case NovaTerm::Mcp::AccessStore::SaveFailure::None: break;
    }
    return SaveFailure::None;
}

void SettingsController::clearSaveFailure()
{
    if (_service)
        _service->access().clearSaveFailure();
}

bool SettingsController::elevationCredentialsPersistent() const
{
    return _service && _service->access().elevationCredentialsPersistent();
}

bool SettingsController::hasElevationCredential(const SessionDirectory::Entry& entry,
                                                const QString& method) const
{
    return _service && _service->access().hasElevationCredential(entry, method);
}

bool SettingsController::configureElevationCredentialAsync(
    const SessionDirectory::Entry& entry, const QString& method, QByteArray secret,
    bool remove, std::function<void(bool)> finished)
{
    return _service
        && _service->access().configureElevationCredentialAsync(
            entry, method, std::move(secret), remove, std::move(finished));
}

} // namespace NovaTerm::Ui
