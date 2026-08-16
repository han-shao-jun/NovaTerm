/**
 * @file   MainWindow.h
 * @brief  无边框主窗口（ElaWindow）。
 *
 * 左侧导航栏已禁用；标题栏仅显示应用 Logo 和一个菜单按钮，其弹出菜单可
 * 导航至会话 / 设置 / 关于。设置和关于以对话框形式打开，而非导航页面。
 * 持有终端页面并连接实时语言切换。
 */
#pragma once
#include "ElaWindow.h"
#include "ui/pages/TerminalPage.h"
#include <ElaContentDialog.h>
#include <ElaIconButton.h>
#include <ElaMenu.h>
#include <ElaToolTip.h>
#include <QByteArray>
#include <optional>
#include "session/SessionTypes.h"

class ElaDialog;
class QCloseEvent;
class QDockWidget;
class QShowEvent;
class SessionPanel;
class SftpPanel;
class SystemMonitorPanel;
class QWidget;

/**
 * @brief 无边框主窗口。
 *
 * 标题栏单图标 → 弹出菜单导航；会话/设置/关于均以对话框形式打开。
 */
class MainWindow : public ElaWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    enum class DockResizeKind {
        None,
        Width,
        Height
    };

    void initWindow();
    void retranslateUi();
    void saveWindowLayout();
    void updateDockResizeHighlight(const QPoint& position);

    // ── 标题栏菜单（单个图标 → 弹出菜单）──
    ElaIconButton* _menuButton{nullptr};
    ElaIconButton* _newSessionButton{nullptr};
    ElaToolTip* _menuTip{nullptr};
    ElaToolTip* _newSessionTip{nullptr};
    ElaMenu* _mainMenu{nullptr};
    ElaMenu* _newSessionMenu{nullptr};
    QAction* _actSession{nullptr};
    QAction* _toggleSftpPanelAction{nullptr};
    QAction* _toggleSystemMonitorAction{nullptr};
    QAction* _actSettings{nullptr};
    QAction* _actAbout{nullptr};
    QAction* _localSessionAction{nullptr};
    QAction* _sshSessionAction{nullptr};
    QAction* _serialSessionAction{nullptr};
    QAction* _telnetSessionAction{nullptr};

    void buildMainMenu();
    void buildNewSessionMenu();

    // 主页面与四区布局：终端固定中央，会话固定左侧，SFTP/监视可停靠。
    TerminalPage* _terminalPage{nullptr};
    SessionPanel* _sessionPanel{nullptr};
    SftpPanel* _sftpPanel{nullptr};
    SystemMonitorPanel* _systemMonitorPanel{nullptr};
    QDockWidget* _sessionDock{nullptr};
    QDockWidget* _sftpDock{nullptr};
    QDockWidget* _systemMonitorDock{nullptr};
    // 原生分隔条较窄，仅悬停在实际相邻区域的调整锚点时显示主题强调色。
    QWidget* _dockResizeHighlight{nullptr};
    // 左键拖动分隔条时锁定调整方向，避免布局变化导致提示短暂消失。
    DockResizeKind _activeDockResizeKind{DockResizeKind::None};
    bool _windowLayoutSaved{false};
    // ElaWindow 在首次显示时还会完成一次内部布局，保留状态用于显示后复原。
    QByteArray _dockStateForFirstShow;
    bool _dockStateRestoredAfterShow{false};

    // ── 会话选择器（ElaDialog）──
    // 仅保持当前活跃的选择器；每次接受/拒绝后重建。
    struct LocalSessionParameters
    {
        TerminalView::LocalShellType type{TerminalView::LocalShellType::Cmd};
        QString wslDistribution;
        QString label;
    };

    ElaDialog* _sessionDialog{nullptr};
    std::optional<LocalSessionParameters> _pendingLocalSession;
    std::optional<SerialConfig> _pendingSerialSession;
    std::optional<SshConfig> _pendingSshSession;
    void showSessionDialog();
    void showSessionDialog(TransportKind initialKind);
    void editSession(const SessionId& id, const RuntimeConfig& runtime,
                     const QByteArray& secret);
    void runSessionDialog(TransportKind initialKind,
                          const std::optional<SessionId>& editingSessionId,
                          const std::optional<RuntimeConfig>& initialConfig,
                          const QByteArray& secret);

    // ── 设置（模态 ElaDialog，内嵌现有 SettingsPage）──
    void showSettingsDialog();

    // ── 关于（模态对话框，行为保持原有逻辑）──
    ElaContentDialog* _aboutDialog{nullptr};
    void showAboutDialog();

public:
    /**
     * @brief 处理标题栏命中测试（Q_INVOKABLE，供导航命中测试回调）。
     * @return true 表示命中可拖拽区域。
     */
    Q_INVOKABLE bool processHitTest();
};
