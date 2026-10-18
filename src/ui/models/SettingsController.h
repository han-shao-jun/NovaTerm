/**
 * @file SettingsController.h
 * @brief 设置界面的只读快照装配与单项授权意图提交。
 *
 * 对话框只连接控件、模型与本控制器；绑定/版本校验、能力解析和授权事务
 * 都在服务端完成。控制器不缓存可变 Session 指针。
 */
#pragma once

#include "SessionPermissionModel.h"
#include "mcp/security/ClientRegistry.h"
#include "session/SessionDirectory.h"

#include <QPointer>
#include <QString>

#include <functional>

#include <optional>

namespace NovaTerm::Mcp {
class Service;
}

namespace NovaTerm::Ui {

class SettingsController
{
public:
    enum class Status
    {
        Applied,
        Stale,
        Rejected,
        Unavailable,
    };

    struct Outcome
    {
        Status status{Status::Unavailable};
        SessionPermissionRow row; ///< Applied 时为更新后的快照。
        QString reason;           ///< 拒绝原因，仅用于本机提示。
    };

    /** @brief 与 AccessStore 一致的持久化失败分类；界面不接触存储对象。 */
    enum class SaveFailure
    {
        None,
        Directory,
        Locked,
        Revision,
        Write,
    };

    explicit SettingsController(NovaTerm::Mcp::Service* service);

    /** @brief 为指定客户端装配只读行快照。 */
    [[nodiscard]] QVector<SessionPermissionRow> rows(const QString& clientId) const;

    /**
     * @brief 提交一项授权选择。
     *
     * 先用稳定键重验当前绑定；过期行返回 `Stale`，界面应排队刷新而不是把
     * 旧行的撤销/授权写到新连接。
     */
    [[nodiscard]] Outcome submit(const SessionPermissionRow& row,
        NovaTerm::Mcp::PermissionKind kind,
        std::optional<NovaTerm::Mcp::DiagnosticId> diagnostic, bool selected) const;

    // 本机接入配置入口；界面不再直接持有可变 AccessStore。
    [[nodiscard]] bool enabled() const;
    bool setEnabled(bool enabled);
    [[nodiscard]] QList<NovaTerm::Mcp::ClientRecord> clients() const;
    [[nodiscard]] QString addClient(const QString& label);
    bool removeClient(const QString& id);
    bool rotateToken(const QString& id);
    [[nodiscard]] SaveFailure lastSaveFailure() const;
    void clearSaveFailure();
    [[nodiscard]] bool elevationCredentialsPersistent() const;
    [[nodiscard]] bool hasElevationCredential(const SessionDirectory::Entry& entry,
                                              const QString& method) const;
    bool configureElevationCredentialAsync(const SessionDirectory::Entry& entry,
        const QString& method, QByteArray secret, bool remove,
        std::function<void(bool)> finished);

private:
    QPointer<NovaTerm::Mcp::Service> _service;
};

} // namespace NovaTerm::Ui
