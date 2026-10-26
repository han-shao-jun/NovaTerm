/**
 * @file SessionPermissionModel.h
 * @brief 设置界面的只读授权行快照、稳定 RowKey 与列映射。
 *
 * 行只保存标识、绑定与能力快照，不保存可解引用的 Session 指针；界面按
 * 具名列读取，委托与处理函数不再使用裸数字 3/6/7 表示业务权限。
 */
#pragma once

#include "mcp/application/CapabilityResolver.h"
#include "mcp/contracts/Permissions.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <optional>

namespace NovaTerm::Ui {

/** @brief 一行会话的稳定键；重连/换绑后与旧行不相等。 */
struct SessionRowKey
{
    QString sessionId;
    QString attachmentId;
    QString epoch;
    quint64 generation{0};
    QString targetFingerprint;

    friend bool operator==(const SessionRowKey& left, const SessionRowKey& right)
    {
        return left.sessionId == right.sessionId
            && left.attachmentId == right.attachmentId
            && left.epoch == right.epoch
            && left.generation == right.generation
            && left.targetFingerprint == right.targetFingerprint;
    }
};

/** @brief 固定诊断子行。 */
struct DiagnosticRow
{
    QString id;
    QString title;
    QString preview; ///< 终端/本地 helper 预览，供提示文字使用。
    bool granted{false};
};

/** @brief 一行的只读快照。 */
struct SessionPermissionRow
{
    SessionRowKey key;
    NovaTerm::Mcp::ClientId client;
    QString title;
    QString state;
    NovaTerm::Mcp::PermissionSelection selection;
    NovaTerm::Mcp::GrantRevision revision;
    NovaTerm::Mcp::SessionBinding binding;
    NovaTerm::Mcp::CapabilitySnapshot capability;
    QVector<DiagnosticRow> diagnostics;
    bool localShell{false};
    QString terminalOwner;       ///< 当前终端控制者 client id；空表示空闲。
    QString controlState;        ///< idle/controlled/preempted 等控制状态。
    QString commandState;        ///< 当前交互命令状态；空表示无命令。
    bool elevationSelectable{false};
    bool clearTerminalSelectable{false};
    bool sudoSaved{false};
    bool suSaved{false};
    bool credentialsPersistent{false};
};

class SessionPermissionModel
{
public:
    /** @brief 列枚举；与对话框表头顺序一致。 */
    enum class Column
    {
        Session = 0,
        State,
        ReadOutput,
        Diagnostics,
        Script,
        Interactive,
        TerminalInput,
        Elevation,
        ClearTerminal,
        Credentials,
        Operation,
        Count,
    };

    static constexpr int ColumnCount = int(Column::Count);

    static constexpr int index(Column column) { return int(column); }
    /** @brief 该列对应的独立权限；诊断列与文本列返回空。 */
    [[nodiscard]] static std::optional<NovaTerm::Mcp::PermissionKind> permissionForColumn(
        int column);
    /** @brief 是否为可勾选的业务权限列（读取/诊断/脚本/交互/完整输入/提权/清空）。 */
    [[nodiscard]] static bool isPermissionColumn(int column);

    /** @brief 用新快照替换整表；按 sessionId 保留展开状态。 */
    void rebuild(QVector<SessionPermissionRow> rows, const QSet<QString>& expanded);
    [[nodiscard]] const QVector<SessionPermissionRow>& rows() const { return _rows; }
    [[nodiscard]] const SessionPermissionRow* find(const QString& sessionId) const;
    /** @brief 单项事务成功后同步新版本，避免下一次点击被判过期。 */
    bool replaceRow(const SessionPermissionRow& row);
    [[nodiscard]] bool isExpanded(const QString& sessionId) const;

private:
    QVector<SessionPermissionRow> _rows;
    QSet<QString> _expanded;
};

} // namespace NovaTerm::Ui
