/**
 * @file   SessionPanel.h
 * @brief  会话面板：历史会话树与重连/编辑入口。
 *
 * 通过 SessionStore 持久化会话历史，QTreeWidget 展示活跃与历史会话。
 * 右键菜单支持重连、编辑、删除。可折叠以腾出终端空间。
 */
#pragma once

#include "session/SessionTypes.h"
#include "ui/terminal/TerminalView.h"

#include <QByteArray>
#include <QWidget>
#include <memory>

class CredentialStore;
class ElaIconButton;
class QTreeWidget;
class QTreeWidgetItem;
class QResizeEvent;
class SessionStore;

/**
 * @brief 会话面板控件：历史会话树 + 重连/编辑。
 */
class SessionPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SessionPanel(QWidget* parent = nullptr);
    ~SessionPanel() override;

    void recordLocal(TerminalView::LocalShellType type,
                     const QString& label = {});
    void recordSerial(const SerialConfig& config);
    void recordSsh(const SshConfig& config);
    void updateLocal(const SessionId& id, TerminalView::LocalShellType type,
                     const QString& label);
    void updateSerial(const SessionId& id, const SerialConfig& config);
    void updateSsh(const SessionId& id, const SshConfig& config);
    void setDockArea(Qt::DockWidgetArea area);
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const noexcept { return _expanded; }

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void localReconnectRequested(TerminalView::LocalShellType type);
    void serialReconnectRequested(const SerialConfig& config);
    void sshReconnectRequested(const SshConfig& config);
    void editSessionRequested(const SessionId& id,
                              const RuntimeConfig& runtime,
                              const QByteArray& secret);
    void reconnectUnavailable(const QString& message);
    void panelWidthChangeRequested(int width);

private:
    void updateToggleButton();
    void repositionToggleButton();
    void retranslateUi();
    void rebuildTree();
    void showItemContextMenu(const QPoint& position);
    void editItem(QTreeWidgetItem* item);
    void deleteItem(QTreeWidgetItem* item);
    void reconnectItem(QTreeWidgetItem* item);
    void upsert(RuntimeConfig runtime, const QByteArray& secret = {});
    void replace(const SessionId& id, RuntimeConfig runtime,
                 const QByteArray& secret = {});
    void saveHistory();
    [[nodiscard]] QString runtimeKey(const RuntimeConfig& runtime) const;

    ElaIconButton* _toggleButton{nullptr};
    QTreeWidget* _tree{nullptr};
    Qt::DockWidgetArea _dockArea{Qt::LeftDockWidgetArea};
    bool _expanded{true};
    int _expandedWidth{280};
    QList<SessionRestoreMetadata> _entries;
    std::unique_ptr<SessionStore> _store;
    std::unique_ptr<CredentialStore> _credentials;
};
