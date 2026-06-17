#pragma once
#include "ElaWindow.h"

class ElaContentDialog;
class ElaDialog;
class ElaIconButton;
class ElaMenu;
class ElaText;
class ElaToolTip;
class QAction;

class TerminalPage;
class SettingsPage;

// 无边框主窗口（ElaWindow）。左侧导航栏已禁用；标题栏仅显示应用
// Logo 和一个菜单按钮，其弹出菜单可导航至 会话 / 设置 / 关于。
// 设置和关于以对话框形式打开，而非导航页面。
// 持有终端页面并连接实时语言切换。
class MainWindow : public ElaWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void changeEvent(QEvent* event) override;

private:
    void initWindow();
    void initContent();
    void retranslateUi();

    // ── 标题栏菜单（单个图标 → 弹出菜单）──
    ElaIconButton* _menuButton{nullptr};
    ElaToolTip* _menuTip{nullptr};
    ElaMenu* _mainMenu{nullptr};
    QAction* _actSession{nullptr};
    QAction* _actSettings{nullptr};
    QAction* _actAbout{nullptr};

    void buildMainMenu();

    // 页面
    TerminalPage* _terminalPage{nullptr};
    QString _terminalKey;

    // ── 会话选择器（ElaDialog）──
    void showSessionDialog();

    // ── 设置（模态 ElaDialog，内嵌现有 SettingsPage）──
    void showSettingsDialog();

    // ── 关于（模态对话框，行为保持原有逻辑）──
    ElaContentDialog* _aboutDialog{nullptr};
    void showAboutDialog();

public:
    Q_INVOKABLE bool processHitTest();
};
