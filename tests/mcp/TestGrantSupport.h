/** @file TestGrantSupport.h
 *  @brief 测试专用的整组授权搭建辅助。
 *
 * 产品代码只有带绑定与预期版本的单项 `changePermission`；测试需要一次把
 * 多个权限摆到指定状态，因此在这里按单项事务逐条提交。`confirmedCommand`
 * 参数只为少改调用点保留，已无对应字段，始终忽略。
 */
#pragma once

#include "mcp/McpAccess.h"
#include "session/SessionDirectory.h"

#include <QSet>
#include <QString>

#include <optional>

namespace NovaTerm::Mcp::TestSupport {

inline std::optional<GrantSnapshot> snapshot(AccessStore& access, const QString& client,
                                             const SessionDirectory::Entry& entry)
{
    if (client.isEmpty())
        return std::nullopt;
    return access.grantSnapshot(ClientId{client}, entry);
}

inline bool applyOne(AccessStore& access, const QString& client,
                     const SessionDirectory::Entry& entry, PermissionKind kind,
                     std::optional<DiagnosticId> diagnostic, bool selected)
{
    const auto current = snapshot(access, client, entry);
    if (!current)
        return false;
    PermissionChange change;
    change.client = ClientId{client};
    change.binding = current->binding;
    change.expectedRevision = current->revision;
    change.kind = kind;
    change.diagnostic = diagnostic;
    change.selected = selected;
    return !isError(access.changePermission(change, entry));
}

inline bool setKind(AccessStore& access, const QString& client,
                    const SessionDirectory::Entry& entry, PermissionKind kind,
                    std::optional<DiagnosticId> diagnostic, bool selected)
{
    const auto current = snapshot(access, client, entry);
    if (!current)
        return false;
    bool value = false;
    switch (kind) {
    case PermissionKind::ReadOutput: value = current->selection.read; break;
    case PermissionKind::Diagnostic:
        value = diagnostic && current->selection.diagnostics.contains(*diagnostic);
        break;
    case PermissionKind::RiskControlledTerminalCommand:
        value = current->selection.riskControlledCommand;
        break;
    case PermissionKind::ScriptPublishAndInvoke: value = current->selection.script; break;
    case PermissionKind::FullTerminalInput: value = current->selection.fullTerminalInput; break;
    case PermissionKind::ManagedElevation: value = current->selection.elevation; break;
    case PermissionKind::ClearTerminal: value = current->selection.clearTerminal; break;
    }
    if (value == selected)
        return true;
    return applyOne(access, client, entry, kind, diagnostic, selected);
}

/** @brief 把选择集合摆到指定状态；返回 false 表示某一步被服务端拒绝。 */
inline bool applySelection(AccessStore& access, const QString& client,
                           const SessionDirectory::Entry& entry,
                           const PermissionSelection& desired)
{
    if (!setKind(access, client, entry, PermissionKind::ReadOutput, std::nullopt,
                 desired.read))
        return false;
    if (!desired.read)
        return true;
    QSet<DiagnosticId> unionIds = desired.diagnostics;
    if (const auto current = snapshot(access, client, entry))
        unionIds.unite(current->selection.diagnostics);
    for (const auto& id : unionIds) {
        if (!setKind(access, client, entry, PermissionKind::Diagnostic, id,
                     desired.diagnostics.contains(id)))
            return false;
    }
    if (!setKind(access, client, entry, PermissionKind::ScriptPublishAndInvoke,
                 std::nullopt, desired.script))
        return false;
    if (!setKind(access, client, entry, PermissionKind::RiskControlledTerminalCommand,
                 std::nullopt, desired.riskControlledCommand))
        return false;
    if (!setKind(access, client, entry, PermissionKind::FullTerminalInput,
                 std::nullopt, desired.fullTerminalInput))
        return false;
    if (!setKind(access, client, entry, PermissionKind::ManagedElevation,
                 std::nullopt, desired.elevation))
        return false;
    return setKind(access, client, entry, PermissionKind::ClearTerminal,
                   std::nullopt, desired.clearTerminal);
}

/** @brief 旧 `setGrant(read, commands, interactive, confirmed, script)` 语义。 */
inline bool setGrant(AccessStore& access, const QString& client,
                     const SessionDirectory::Entry& entry, bool read,
                     QSet<QString> commands = {}, bool interactive = false,
                     bool confirmed = false, bool script = false)
{
    static_cast<void>(confirmed); // 已删除的死字段，不再参与授权。
    PermissionSelection desired;
    desired.read = read;
    for (const auto& id : commands)
        desired.diagnostics.insert(DiagnosticId{id});
    desired.riskControlledCommand = interactive;
    desired.script = script;
    if (read) {
        // 旧接口只在 attachment/epoch/目标指纹都相同时保留完整输入与提权。
        const auto current = snapshot(access, client, entry);
        if (current && current->binding.epoch.str() == entry.epoch
            && current->binding.attachment.str() == entry.attachmentId
            && current->binding.targetFingerprint.value_or(QString{})
                == entry.targetFingerprint) {
            desired.fullTerminalInput = current->selection.fullTerminalInput;
            desired.elevation = current->selection.elevation;
            desired.clearTerminal = current->selection.clearTerminal;
        }
    }
    return applySelection(access, client, entry, desired);
}

/** @brief 旧 `setTerminalGrant(control, elevation)` 语义。 */
inline bool setTerminalGrant(AccessStore& access, const QString& client,
                             const SessionDirectory::Entry& entry, bool control,
                             bool elevation = false)
{
    if (elevation && !control)
        return false;
    const auto current = snapshot(access, client, entry);
    if (!current || !current->selection.read)
        return false;
    PermissionSelection desired = current->selection;
    desired.fullTerminalInput = control;
    desired.elevation = control && elevation;
    return applySelection(access, client, entry, desired);
}

} // namespace NovaTerm::Mcp::TestSupport
