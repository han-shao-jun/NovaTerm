/** @file SessionPermissionModel.cpp @brief 列映射与只读行快照实现。 */
#include "SessionPermissionModel.h"

namespace NovaTerm::Ui {

std::optional<NovaTerm::Mcp::PermissionKind> SessionPermissionModel::permissionForColumn(
    int column)
{
    using NovaTerm::Mcp::PermissionKind;
    switch (column) {
    case index(Column::ReadOutput): return PermissionKind::ReadOutput;
    case index(Column::TerminalInput): return PermissionKind::FullTerminalInput;
    case index(Column::Elevation): return PermissionKind::ManagedElevation;
    case index(Column::ClearTerminal): return PermissionKind::ClearTerminal;
    default: return std::nullopt;
    }
}

bool SessionPermissionModel::isPermissionColumn(int column)
{
    return column == index(Column::ReadOutput)
        || column == index(Column::TerminalInput) || column == index(Column::Elevation)
        || column == index(Column::ClearTerminal);
}

void SessionPermissionModel::rebuild(QVector<SessionPermissionRow> rows,
                                     const QSet<QString>& expanded)
{
    _expanded = expanded;
    _rows = std::move(rows);
}

const SessionPermissionRow* SessionPermissionModel::find(const QString& sessionId) const
{
    for (const auto& row : _rows)
        if (row.key.sessionId == sessionId)
            return &row;
    return nullptr;
}

bool SessionPermissionModel::replaceRow(const SessionPermissionRow& row)
{
    for (auto& current : _rows) {
        if (current.key.sessionId != row.key.sessionId)
            continue;
        current = row;
        return true;
    }
    return false;
}

bool SessionPermissionModel::isExpanded(const QString& sessionId) const
{
    return _expanded.contains(sessionId);
}

} // namespace NovaTerm::Ui
