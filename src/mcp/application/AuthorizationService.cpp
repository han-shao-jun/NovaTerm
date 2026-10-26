/** @file AuthorizationService.cpp @brief 单项授权事务与级联规则的唯一实现。 */
#include "AuthorizationService.h"

#include <optional>
#include <utility>

namespace NovaTerm::Mcp {

AuthorizationService::AuthorizationService(AuthorizationPorts ports)
    : _ports(std::move(ports))
{
}

bool AuthorizationService::sameBinding(const SessionBinding& left,
                                       const SessionBinding& right)
{
    return left.session == right.session
        && left.attachment == right.attachment
        && left.epoch == right.epoch
        && left.generation == right.generation
        && left.targetFingerprint == right.targetFingerprint;
}

bool AuthorizationService::executionBindingMatches(const SessionBinding& row,
                                                   const SessionBinding& current)
{
    // 读取按 attachment 作用域保留；执行类权限只对同一连接世代有效。
    return row.epoch == current.epoch
        && row.generation == current.generation
        && row.targetFingerprint == current.targetFingerprint;
}

bool AuthorizationService::readyForInput(const GrantFacts& facts)
{
    return facts.sessionRunning
        && facts.transportConnected
        && facts.transportMatchesSession
        && facts.generationMatches
        && !facts.binding.epoch.isEmpty()
        && !facts.binding.attachment.isEmpty();
}

GrantSnapshot AuthorizationService::snapshot(const ClientId& client,
                                             const GrantFacts& facts) const
{
    GrantSnapshot view;
    view.client = client;
    view.binding = facts.binding;
    if (_ports.clientVersion)
        view.accessVersion = _ports.clientVersion(client);
    const auto row = _store.find(client, facts.binding.session);
    if (!row || facts.binding.session.isEmpty()
        || row->binding.attachment != facts.binding.attachment)
        return view;
    view.revision = row->revision;
    view.selection = row->selection;
    if (!executionBindingMatches(row->binding, facts.binding)) {
        view.selection.diagnostics.clear();
        view.selection.riskControlledCommand = false;
        view.selection.script = false;
        view.selection.fullTerminalInput = false;
        view.selection.elevation = false;
        view.selection.clearTerminal = false;
    }
    return view;
}

Result<GrantSnapshot, AuthorizationError> AuthorizationService::change(
    const PermissionChange& change, const GrantFacts& facts)
{
    const auto current = snapshot(change.client, facts);
    const auto reject = [&current](AuthorizationRejection reason) {
        return Result<GrantSnapshot, AuthorizationError>{
            AuthorizationError{reason, current}};
    };

    if (!_ports.enabled || !_ports.enabled())
        return reject(AuthorizationRejection::Disabled);
    if (!_ports.knownClient || !_ports.knownClient(change.client))
        return reject(AuthorizationRejection::UnknownClient);
    if (!sameBinding(change.binding, facts.binding))
        return reject(AuthorizationRejection::StaleBinding);

    const auto row = _store.find(change.client, facts.binding.session);
    const bool rowApplies = row.has_value() && !facts.binding.session.isEmpty()
        && row->binding.attachment == facts.binding.attachment;
    const quint64 expected = rowApplies ? row->revision.value() : 0;
    if (change.expectedRevision.value() != expected)
        return reject(AuthorizationRejection::StaleRevision);

    PermissionSelection selection = rowApplies ? row->selection : PermissionSelection{};
    if (rowApplies && !executionBindingMatches(row->binding, facts.binding)) {
        // 同一目标重连后，界面再次提交的修改以新连接为准：读取保留，
        // 诊断/脚本/交互/完整输入/提权/清空必须重新授权。
        selection.diagnostics.clear();
        selection.riskControlledCommand = false;
        selection.script = false;
        selection.fullTerminalInput = false;
        selection.elevation = false;
        selection.clearTerminal = false;
    }

    AuthorizationRejection invalid = AuthorizationRejection::None;
    switch (change.kind) {
    case PermissionKind::ReadOutput:
        selection.read = change.selected;
        if (!change.selected) {
            selection.diagnostics.clear();
            selection.riskControlledCommand = false;
            selection.script = false;
            selection.fullTerminalInput = false;
            selection.elevation = false;
            selection.clearTerminal = false;
        }
        break;
    case PermissionKind::Diagnostic:
        if (!selection.read || !change.diagnostic) {
            invalid = AuthorizationRejection::InvalidSelection;
            break;
        }
        if (change.selected) {
            if (!_ports.knownDiagnostic || !_ports.knownDiagnostic(*change.diagnostic)) {
                invalid = AuthorizationRejection::InvalidSelection;
                break;
            }
            selection.diagnostics.insert(*change.diagnostic);
        } else {
            selection.diagnostics.remove(*change.diagnostic);
        }
        break;
    case PermissionKind::RiskControlledTerminalCommand:
        if (!selection.read) {
            invalid = AuthorizationRejection::InvalidSelection;
            break;
        }
        selection.riskControlledCommand = change.selected;
        break;
    case PermissionKind::ScriptPublishAndInvoke:
        if (!selection.read) {
            invalid = AuthorizationRejection::InvalidSelection;
            break;
        }
        selection.script = change.selected;
        break;
    case PermissionKind::FullTerminalInput:
        if (change.selected) {
            if (!readyForInput(facts)) {
                invalid = AuthorizationRejection::SessionNotReady;
                break;
            }
            // 现有界面语义：开启完整输入同时选择读取共享。
            selection.read = true;
            selection.fullTerminalInput = true;
        } else {
            selection.fullTerminalInput = false;
            selection.elevation = false;
        }
        break;
    case PermissionKind::ManagedElevation:
        if (change.selected) {
            if (!selection.fullTerminalInput) {
                invalid = AuthorizationRejection::InvalidSelection;
                break;
            }
            if (!readyForInput(facts)) {
                invalid = AuthorizationRejection::SessionNotReady;
                break;
            }
            if (_ports.credentialUpdatePending && _ports.credentialUpdatePending()) {
                invalid = AuthorizationRejection::CredentialsPending;
                break;
            }
            selection.elevation = true;
        } else {
            selection.elevation = false;
        }
        break;
    case PermissionKind::ClearTerminal:
        if (change.selected) {
            if (!selection.read) {
                invalid = AuthorizationRejection::InvalidSelection;
                break;
            }
            if (!readyForInput(facts)) {
                invalid = AuthorizationRejection::SessionNotReady;
                break;
            }
        }
        selection.clearTerminal = change.selected;
        break;
    }
    if (invalid != AuthorizationRejection::None)
        return reject(invalid);

    GrantRow next;
    next.binding = facts.binding;
    next.selection = selection;
    next.revision = GrantRevision{(rowApplies ? row->revision.value() : 0) + 1};
    _store.save(change.client, facts.binding.session, next);

    GrantSnapshot applied;
    applied.client = change.client;
    applied.binding = facts.binding;
    applied.revision = next.revision;
    applied.accessVersion = _ports.advanceClientVersion
        ? _ports.advanceClientVersion(change.client) : current.accessVersion;
    applied.selection = selection;
    return applied;
}

std::optional<GrantRow> AuthorizationService::row(const ClientId& client,
                                                  const SessionId& session) const
{
    return _store.find(client, session);
}

void AuthorizationService::removeSession(const SessionId& session)
{
    _store.removeSession(session);
}

void AuthorizationService::removeClient(const ClientId& client)
{
    _store.removeClient(client);
}

void AuthorizationService::clear() { _store.clear(); }

std::size_t AuthorizationService::size() const { return _store.size(); }

std::vector<std::pair<ClientId, SessionId>> AuthorizationService::revokeElevation(
    const QString& targetFingerprint)
{
    std::vector<std::pair<ClientId, SessionId>> revoked;
    for (const auto& entry : _store.entries()) {
        const auto fingerprint = entry.row.binding.targetFingerprint.value_or(QString{});
        if (fingerprint != targetFingerprint || !entry.row.selection.elevation)
            continue;
        auto row = entry.row;
        row.selection.elevation = false;
        row.revision = GrantRevision{row.revision.value() + 1};
        _store.save(entry.client, entry.session, row);
        revoked.emplace_back(entry.client, entry.session);
    }
    return revoked;
}

std::vector<std::pair<ClientId, SessionId>> AuthorizationService::revokeAllElevation()
{
    std::vector<std::pair<ClientId, SessionId>> revoked;
    for (const auto& entry : _store.entries()) {
        if (!entry.row.selection.elevation)
            continue;
        auto row = entry.row;
        row.selection.elevation = false;
        row.revision = GrantRevision{row.revision.value() + 1};
        _store.save(entry.client, entry.session, row);
        revoked.emplace_back(entry.client, entry.session);
    }
    return revoked;
}

} // namespace NovaTerm::Mcp
