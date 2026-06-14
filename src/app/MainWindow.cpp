#include "MainWindow.h"
#include "ElaDialog.h"
#include "ElaIconButton.h"
#include "ElaMenu.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaToolTip.h"
#include "ElaTheme.h"
#include "ElaContentDialog.h"
#include "ElaMessageBar.h"
#include "ElaApplication.h"
#include "pages/TerminalPage.h"
#include "pages/SettingsPage.h"
#include "core/LanguageManager.h"
#include "core/ConfigManager.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget* parent) : ElaWindow(parent)
{
    initWindow();
    initContent();

    // 语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    // 关闭确认对话框
    auto* closeDialog = new ElaContentDialog(this);
    connect(closeDialog, &ElaContentDialog::rightButtonClicked,
            this, &MainWindow::close);
    connect(closeDialog, &ElaContentDialog::middleButtonClicked, this, [=]() {
        closeDialog->close();
        showMinimized();
    });
    setIsDefaultClosed(false);
    connect(this, &MainWindow::closeButtonClicked, this, [=]() {
        closeDialog->exec();
    });
}

MainWindow::~MainWindow() = default;

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    }
    ElaWindow::changeEvent(event);
}

void MainWindow::retranslateUi()
{
    setWindowTitle(tr("NovaTerm"));
    if (_centralStack) _centralStack->setText(tr("NovaTerm"));

    if (_menuTip) _menuTip->setToolTip(tr("Menu"));

    // 重新翻译弹出菜单项
    if (_actSession)  _actSession->setText(tr("Session"));
    if (_actSettings) _actSettings->setText(tr("Settings"));
    if (_actAbout)    _actAbout->setText(tr("About"));
}

void MainWindow::initWindow()
{
    setWindowTitle(tr("NovaTerm"));

    int w = ConfigManager::get<int>("window.width", 1280);
    int h = ConfigManager::get<int>("window.height", 800);
    resize(w, h);

    // 隐藏用户信息卡片
    setUserInfoCardVisible(false);

    // ── 标题栏左侧：仅显示应用 Logo + 菜单按钮 ──
    // 保留窗口图标用于任务栏 / Alt-Tab；标题栏上可见的 Logo 由下方的
    // QLabel 自行绘制，以便控制其尺寸。
    setWindowIcon(QIcon(":/icons/app_logo.svg"));

    // 移除默认的左侧控件：
    //   • 导航切换按钮（ElaWindow 默认启用，但我们的导航栏已隐藏）
    //   • 路由前进/后退按钮
    //   • AppBar 内置图标槽（18px，太小）和标题文本
    //     （两者都是 AppBar 的直接子 QLabel；窗口标题本身保留给任务栏）
    setWindowButtonFlag(ElaAppBarType::NavigationButtonHint, false);
    setWindowButtonFlag(ElaAppBarType::RouteBackButtonHint, false);
    setWindowButtonFlag(ElaAppBarType::RouteForwardButtonHint, false);
    if (auto* appBar = findChild<ElaAppBar*>()) {
        auto hideAppBarLabels = [appBar]() {
            // ElaText 继承自 QLabel，因此同时覆盖图标标签和标题标签。
            const auto labels = appBar->findChildren<QLabel*>(
                QString(), Qt::FindDirectChildrenOnly);
            for (auto* label : labels)
                label->hide();
        };
        hideAppBarLabels();
        // AppBar 在 windowTitleChanged 时会重新显示标题标签；需要重新隐藏。
        connect(this, &QWidget::windowTitleChanged, appBar,
                [hideAppBarLabels](const QString&) { hideAppBarLabels(); });
    }

    // 中央占位区域
    _centralStack = new ElaText(tr("NovaTerm"), this);
    _centralStack->setTextPixelSize(32);
    _centralStack->setAlignment(Qt::AlignCenter);
    addCentralWidget(_centralStack);

    // ═══════════════════════════════════════════════════════════════
    // 菜单图标，放置在 LeftArea 中使其位于 Logo 右侧。
    //   • 悬停  → ElaToolTip "Menu"
    //   • 点击  → 弹出 ElaMenu，包含 会话 / 设置 / 关于
    // ═══════════════════════════════════════════════════════════════
    _menuButton = new ElaIconButton(ElaIconType::Bars, 18, 36, 36, this);
    // ElaToolTip（非原生 QToolTip）：其背景使用主题的 PopupBase/PopupBorder
    // 绘制，并已绑定 eTheme->themeModeChanged，因此自动跟随全局亮色/暗色主题。
    _menuTip = new ElaToolTip(_menuButton);
    _menuTip->setToolTip(tr("Menu"));

    buildMainMenu();
    connect(_menuButton, &QPushButton::clicked, this, [this]() {
        _mainMenu->popup(_menuButton->mapToGlobal(QPoint(0, _menuButton->height() + 2)));
    });

    QWidget* customWidget = new QWidget(this);
    QHBoxLayout* customLayout = new QHBoxLayout(customWidget);
    // 左边距为零，使 Logo 紧贴窗口左边缘
    // （AppBar 自身左侧布局为空 —— 其所有控件均已隐藏）。
    customLayout->setContentsMargins(0, 0, 0, 0);
    customLayout->setSpacing(6);

    // 应用 Logo，尺寸与 36px 菜单按钮匹配（26px 图标放在 26 宽的槽中，
    // 使其左对齐，在 36px 高的标题栏中垂直居中）。
    auto* logoLabel = new QLabel(customWidget);
    logoLabel->setPixmap(QIcon(":/icons/app_logo.svg").pixmap(26, 26));
    logoLabel->setFixedSize(26, 36);
    logoLabel->setAlignment(Qt::AlignCenter);
    customLayout->addWidget(logoLabel);

    customLayout->addWidget(_menuButton);
    customLayout->addStretch();
    setCustomWidget(ElaAppBarType::LeftArea, customWidget, this, "processHitTest");
}

void MainWindow::buildMainMenu()
{
    _mainMenu = new ElaMenu(this);
    _mainMenu->setMenuItemHeight(27);

    // ── 会话：打开会话/连接选择器 ──
    _actSession = _mainMenu->addElaIconAction(ElaIconType::Terminal, tr("Session"));
    connect(_actSession, &QAction::triggered, this, &MainWindow::showSessionDialog);

    // ── 设置：ElaDialog 内嵌现有 SettingsPage ──
    _actSettings = _mainMenu->addElaIconAction(ElaIconType::GearComplex, tr("Settings"));
    connect(_actSettings, &QAction::triggered, this, &MainWindow::showSettingsDialog);

    // ── 关于：模态对话框（行为不变）──
    _actAbout = _mainMenu->addElaIconAction(ElaIconType::CircleInfo, tr("About"));
    connect(_actAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::initContent()
{
    _terminalPage = new TerminalPage(this);

    // 仅注册终端（会话）页面；导航通过标题栏菜单进行，
    // 因此左侧导航栏保持隐藏。
    addPageNode(tr("Terminal"), _terminalPage, ElaIconType::Terminal);
    _terminalKey = _terminalPage->property("ElaPageKey").toString();

    setIsNavigationBarEnable(false);
}

bool MainWindow::processHitTest()
{
    // 自定义区域中除菜单按钮外均可拖动。
    if (!_menuButton)
        return true;
    return !ElaApplication::containsCursorToItem(_menuButton);
}

// ═══════════════════════════════════════════════════════════════════
//  会话选择器 — ElaDialog
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showSessionDialog()
{
    auto* dialog = new ElaDialog(this);
    dialog->setWindowTitle(tr("Session"));
    dialog->setFixedSize(360, 320);
    dialog->setIsFixedSize(true);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);

    auto* central = new QWidget(dialog);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(40, 10, 40, 30);
    layout->setSpacing(12);

    auto* titleText = new ElaText(tr("New Session"), central);
    titleText->setTextPixelSize(20);
    layout->addWidget(titleText);

    auto* tipText = new ElaText(tr("Choose a session type"), central);
    tipText->setTextPixelSize(13);
    layout->addWidget(tipText);
    layout->addSpacing(10);

    // 本地终端 — 当前可用（QTermWidget 内置 KPty）
    auto* localButton = new ElaPushButton(tr("Local Shell"), central);
    layout->addWidget(localButton);
    connect(localButton, &QPushButton::clicked, this, [this, dialog]() {
        if (!_terminalKey.isEmpty())
            navigation(_terminalKey);
        dialog->close();
    });

    // 远程协议 — 占位按钮，等待 ITransport 实现完成后可用
    auto* sshButton = new ElaPushButton(tr("SSH"), central);
    layout->addWidget(sshButton);
    auto* serialButton = new ElaPushButton(tr("Serial"), central);
    layout->addWidget(serialButton);
    auto* telnetButton = new ElaPushButton(tr("Telnet"), central);
    layout->addWidget(telnetButton);

    for (ElaPushButton* b : {sshButton, serialButton, telnetButton}) {
        connect(b, &QPushButton::clicked, this, [b]() {
            ElaMessageBar::information(ElaMessageBarType::BottomRight,
                                       b->text(), tr("Not implemented yet"), 2000);
        });
    }

    layout->addStretch();

    auto* mainLayout = new QVBoxLayout(dialog);
    mainLayout->setContentsMargins(0, 25, 0, 0);
    mainLayout->addWidget(central);

    dialog->exec();
    dialog->deleteLater();
}

// ═══════════════════════════════════════════════════════════════════
//  设置 — ElaDialog 内嵌现有 SettingsPage
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showSettingsDialog()
{
    auto* dialog = new ElaDialog(this);
    dialog->setWindowTitle(tr("Settings"));
    dialog->resize(900, 640);
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);

    // 以 MainWindow 作为父对象构造，使窗口相关控件
    // （绘制模式 / 窗口特效 / 导航栏模式 / 页面切换模式）仍能定位到它；
    // 捕获的窗口指针在控件被移入对话框布局后仍然有效。
    auto* settingsPage = new SettingsPage(this);
    settingsPage->setTitleVisible(false);

    auto* mainLayout = new QVBoxLayout(dialog);
    mainLayout->setContentsMargins(0, 30, 0, 0);
    mainLayout->addWidget(settingsPage);

    dialog->exec();
    dialog->deleteLater();
}

// ═══════════════════════════════════════════════════════════════════
//  关于 — 模态对话框（行为保持原有逻辑）
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showAboutDialog()
{
    // 若已打开，直接将其前置
    if (_aboutDialog)
    {
        _aboutDialog->raise();
        _aboutDialog->activateWindow();
        return;
    }

    auto* dialog = new ElaContentDialog(this);
    dialog->setWindowTitle(tr("About"));
    dialog->setRightButtonText(tr("OK"));

    // 隐藏左侧和中间按钮 — 关于对话框仅需一个确定按钮
    auto buttons = dialog->findChildren<ElaPushButton*>();
    if (buttons.size() >= 3)
    {
        buttons[0]->hide(); // 左侧按钮
        buttons[1]->hide(); // 中间按钮
    }

    // ── 构建关于内容 ──
    auto* centralWidget = new QWidget(dialog);
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(40, 40, 40, 40);
    layout->setSpacing(12);

    auto* titleText = new ElaText(tr("NovaTerm"), centralWidget);
    titleText->setTextPixelSize(28);
    layout->addWidget(titleText);

    auto* versionText = new ElaText(tr("Version 0.1.0"), centralWidget);
    versionText->setTextPixelSize(15);
    layout->addWidget(versionText);

    layout->addSpacing(20);

    auto* descText = new ElaText(
        tr("A cross-platform terminal emulator & SSH client\n"
           "with FluentUI design, inspired by WindTerm.\n\n"
           "Built with:\n"
           "  • Qt 6.7  •  ElaWidgetTools (FluentUI)\n"
           "  • QTermWidget (terminal emulation)\n"
           "  • libssh (SSH/SFTP)"),
        centralWidget);
    descText->setTextPixelSize(13);
    layout->addWidget(descText);

    layout->addSpacing(20);

    auto* licenseText = new ElaText(tr("License: GPLv2+"), centralWidget);
    licenseText->setTextPixelSize(12);
    layout->addWidget(licenseText);

    layout->addStretch();

    dialog->setCentralWidget(centralWidget);

    // ── 语言切换支持 ──
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            dialog, [=](const QString&) {
        titleText->setText(MainWindow::tr("NovaTerm"));
        versionText->setText(MainWindow::tr("Version 0.1.0"));
        descText->setText(
            MainWindow::tr("A cross-platform terminal emulator & SSH client\n"
                           "with FluentUI design, inspired by WindTerm.\n\n"
                           "Built with:\n"
                           "  • Qt 6.7  •  ElaWidgetTools (FluentUI)\n"
                           "  • QTermWidget (terminal emulation)\n"
                           "  • libssh (SSH/SFTP)"));
        licenseText->setText(MainWindow::tr("License: GPLv2+"));
        dialog->setWindowTitle(MainWindow::tr("About"));
        dialog->setRightButtonText(MainWindow::tr("OK"));
    });

    // ── 确定按钮：使用 QDialog::done() 避免动画冲突 ──
    connect(dialog, &ElaContentDialog::rightButtonClicked, dialog, [dialog]() {
        dialog->done(QDialog::Accepted);
    });

    // ── 跟踪对话框生命周期 ──
    _aboutDialog = dialog;
    connect(dialog, &QObject::destroyed, this, [this]() {
        _aboutDialog = nullptr;
    });

    dialog->exec();
    dialog->deleteLater();
}
