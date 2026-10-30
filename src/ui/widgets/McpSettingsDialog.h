/** @file McpSettingsDialog.h
 *  @brief MCP 接入配置、会话共享与受限命令授权。
 */
#pragma once
#include "ElaDialog.h"
#include "mcp/contracts/Permissions.h"
#include "ui/models/SessionPermissionModel.h"
#include <QPointer>
#include <memory>
#include <optional>
namespace NovaTerm::Mcp { class Service; }
namespace NovaTerm::Ui { class SettingsController; }
class ElaCheckBox;
class ElaText;
class ElaTreeWidget;
class QTreeWidgetItem;

class McpSettingsDialog final : public ElaDialog
{
    Q_OBJECT
public:
    explicit McpSettingsDialog(NovaTerm::Mcp::Service* service, QWidget* parent);
    ~McpSettingsDialog() override;
private:
    void refresh();
    void reportFailure();
    void editElevationCredentials(const QString& sessionId);
    /** @brief 提交一项授权并回写服务端级联结果；失败时由调用方排队刷新。 */
    bool applyPermission(QTreeWidgetItem* root,
                         const NovaTerm::Ui::SessionPermissionRow& row,
                         NovaTerm::Mcp::PermissionKind kind,
                         std::optional<NovaTerm::Mcp::DiagnosticId> diagnostic,
                         bool selected);
    QPointer<NovaTerm::Mcp::Service> _service;
    std::unique_ptr<NovaTerm::Ui::SessionPermissionModel> _model;
    std::unique_ptr<NovaTerm::Ui::SettingsController> _controller;
    ElaCheckBox* _enabled{nullptr};
    ElaText* _status{nullptr};
    ElaTreeWidget* _sessions{nullptr};
    quint64 _accessIdentity{0};
    bool _refreshing{false};
};
