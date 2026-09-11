/**
 * @file   SessionPanel.h
 * @brief  快捷连接面板：新建会话与分组历史会话入口。
 *
 * 通过 SessionStore 持久化会话历史，ElaTreeWidget 按连接类型展示保存的会话。
 * 条目以图标和两行文本展示，支持名称/主机搜索。右键菜单支持编辑、删除，
 * 双击重连。标题栏按钮可将面板折叠为窄侧栏。
 */
#pragma once

#include "session/SessionTypes.h"
#include "ui/terminal/TerminalView.h"

#include <QByteArray>
#include <QWidget>
#include <memory>

class CredentialStore;
class ElaIconButton;
class ElaLineEdit;
class ElaPushButton;
class ElaTreeWidget;
class QResizeEvent;
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
                     const QString& label = {},
                     const QString& workingDirectory = {});
    void recordSerial(const SerialConfig& config);
    void recordSsh(const SshConfig& config);
    void recordTelnet(const TelnetConfig& config);
    void updateLocal(const SessionId& id, TerminalView::LocalShellType type,
                     const QString& wslDistribution,
                     const QString& label,
                     const QString& workingDirectory);
    void updateSerial(const SessionId& id, const SerialConfig& config);
    void updateSsh(const SessionId& id, const SshConfig& config);
    void updateTelnet(const SessionId& id, const TelnetConfig& config);

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
                                 const QString& label,
                                 const QString& workingDirectory);
    void serialReconnectRequested(const SerialConfig& config);
    void sshReconnectRequested(const SshConfig& config);
    void telnetReconnectRequested(const TelnetConfig& config);
    void editSessionRequested(const SessionId& id,
                              const RuntimeConfig& runtime,
                              const QByteArray& secret);
    void reconnectUnavailable(const QString& message);

private:
    static constexpr int CollapsedWidth = 40;
    // 单列图标、标题和连接详情在常用桌面字体下保持可读所需的最小展开宽度。
    static constexpr int MinimumExpandedWidth = 190;

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
    ElaIconButton* _collapseButton{nullptr};
    ElaPushButton* _newSessionButton{nullptr};
    ElaLineEdit* _searchEdit{nullptr};
    ElaTreeWidget* _tree{nullptr};
    bool _collapsed{false};
    int _expandedWidth{260};
    QList<SessionRestoreMetadata> _entries;
    std::unique_ptr<SessionStore> _store;
    std::unique_ptr<CredentialStore> _credentials;
};
