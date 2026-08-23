/**
 * @file   SessionPanel.h
 * @brief  快捷连接面板：新建会话与分组历史会话入口。
 *
 * 通过 SessionStore 持久化会话历史，QTreeWidget 按连接类型展示保存的会话。
 * 右键菜单支持重连、编辑、删除。标题栏按钮可将面板折叠为窄侧栏。
 */
#pragma once

#include "session/SessionTypes.h"
#include "ui/terminal/TerminalView.h"

#include <QByteArray>
#include <QWidget>
#include <memory>

class CredentialStore;
class ElaIconButton;
class ElaPushButton;
class QLabel;
class QResizeEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
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
                     const QString& wslDistribution,
                     const QString& label = {});
    void recordSerial(const SerialConfig& config);
    void recordSsh(const SshConfig& config);
    void updateLocal(const SessionId& id, TerminalView::LocalShellType type,
                     const QString& wslDistribution,
                     const QString& label);
    void updateSerial(const SessionId& id, const SerialConfig& config);
    void updateSsh(const SessionId& id, const SshConfig& config);

    /** 折叠或展开快捷连接面板；折叠后保留标题栏展开按钮。 */
    void setCollapsed(bool collapsed);
    [[nodiscard]] bool isCollapsed() const noexcept { return _collapsed; }

    /** 设置面板展开时恢复的宽度。 */
    void setExpandedWidth(int width);
    [[nodiscard]] int expandedWidth() const noexcept { return _expandedWidth; }

protected:
    void resizeEvent(QResizeEvent* event) override;

signals:
    void newSessionRequested();
    void collapsedChanged(bool collapsed);
    void panelWidthChangeRequested(int width);
    void localReconnectRequested(TerminalView::LocalShellType type,
                                 const QString& wslDistribution,
                                 const QString& label);
    void serialReconnectRequested(const SerialConfig& config);
    void sshReconnectRequested(const SshConfig& config);
    void editSessionRequested(const SessionId& id,
                              const RuntimeConfig& runtime,
                              const QByteArray& secret);
    void reconnectUnavailable(const QString& message);

private:
    static constexpr int CollapsedWidth = 40;

    void updateCollapsedUi();
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

    QVBoxLayout* _rootLayout{nullptr};
    QLabel* _titleLabel{nullptr};
    ElaIconButton* _collapseButton{nullptr};
    ElaPushButton* _newSessionButton{nullptr};
    QTreeWidget* _tree{nullptr};
    bool _collapsed{false};
    int _expandedWidth{260};
    QList<SessionRestoreMetadata> _entries;
    std::unique_ptr<SessionStore> _store;
    std::unique_ptr<CredentialStore> _credentials;
};
