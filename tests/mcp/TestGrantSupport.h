/** @file TestGrantSupport.h
 *  @brief 测试专用的整组授权搭建辅助。
 *
 * 产品代码只有带绑定与预期版本的单项 `changePermission`；测试需要一次把
 * 多个权限摆到指定状态，因此在这里按单项事务逐条提交。
 */
#pragma once

#include "mcp/McpAccess.h"
#include "session/SessionDirectory.h"

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
                     bool selected)
{
    const auto current = snapshot(access, client, entry);
    if (!current)
        return false;
    PermissionChange change;
    change.client = ClientId{client};
    change.binding = current->binding;
    change.expectedRevision = current->revision;
    change.kind = kind;
    change.selected = selected;
    return !isError(access.changePermission(change, entry));
}

inline bool setKind(AccessStore& access, const QString& client,
                    const SessionDirectory::Entry& entry, PermissionKind kind,
                    bool selected)
{
    const auto current = snapshot(access, client, entry);
    if (!current)
        return false;
    bool value = false;
    switch (kind) {
    case PermissionKind::ReadOutput: value = current->selection.read; break;
    case PermissionKind::FullTerminalInput: value = current->selection.fullTerminalInput; break;
    case PermissionKind::ManagedElevation: value = current->selection.elevation; break;
    case PermissionKind::ClearTerminal: value = current->selection.clearTerminal; break;
    case PermissionKind::RelaxedPermissions: value = current->selection.relaxedPermissions; break;
    }
    if (value == selected)
        return true;
    return applyOne(access, client, entry, kind, selected);
}

/** @brief 把选择集合摆到指定状态；返回 false 表示某一步被服务端拒绝。 */
inline bool applySelection(AccessStore& access, const QString& client,
                           const SessionDirectory::Entry& entry,
                           const PermissionSelection& desired)
{
    if (!setKind(access, client, entry, PermissionKind::ReadOutput,
                 desired.read))
        return false;
    if (!desired.read)
        return true;
    if (!setKind(access, client, entry, PermissionKind::FullTerminalInput,
                 desired.fullTerminalInput))
        return false;
    if (!setKind(access, client, entry, PermissionKind::ManagedElevation,
                 desired.elevation))
        return false;
    if (!setKind(access, client, entry, PermissionKind::RelaxedPermissions,
                 desired.relaxedPermissions))
        return false;
    return setKind(access, client, entry, PermissionKind::ClearTerminal,
                   desired.clearTerminal);
}

/** @brief 设置读取共享，并保留当前连接上的其他授权。 */
inline bool setGrant(AccessStore& access, const QString& client,
                     const SessionDirectory::Entry& entry, bool read)
{
    PermissionSelection desired;
    desired.read = read;
    if (read) {
        // 快照仅保留当前连接上的完整输入、提权与清屏权限。
        const auto current = snapshot(access, client, entry);
        if (current && current->binding.epoch.str() == entry.epoch
            && current->binding.attachment.str() == entry.attachmentId
            && current->binding.targetFingerprint.value_or(QString{})
                == entry.targetFingerprint) {
            desired.fullTerminalInput = current->selection.fullTerminalInput;
            desired.elevation = current->selection.elevation;
            desired.clearTerminal = current->selection.clearTerminal;
            desired.relaxedPermissions = current->selection.relaxedPermissions;
        }
    }
    return applySelection(access, client, entry, desired);
}

/** @brief 在读取共享基础上设置完整输入与托管提权。 */
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
    desired.relaxedPermissions = control && desired.relaxedPermissions;
    desired.elevation = control && elevation;
    return applySelection(access, client, entry, desired);
}

} // namespace NovaTerm::Mcp::TestSupport
