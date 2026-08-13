/**
 * @file   MainWindow.cpp
 * @brief  主窗口实现：菜单构建、会话对话框与语言切换。
 *
 * 构建标题栏图标菜单，连接 TerminalPage 的会话请求信号弹出 SessionPage
 * 对话框。changeEvent 处理 LanguageChange 时调用 retranslateUi() 刷新菜单文本。
 */
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
#include "ElaTabWidget.h"
#include "ElaTabBar.h"
#include "ui/pages/TerminalPage.h"
#include "ui/pages/SettingsPage.h"
#include "ui/pages/SessionPage.h"
#include "ui/pages/AboutPage.h"
#include "ui/widgets/SessionPanel.h"
#include "service/LanguageManager.h"
#include "service/ConfigManager.h"
#include <QApplication>
#include <QDockWidget>
#include <QEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QVBoxLayout>
#include <QThread>
#include <QTimer>

namespace {

class DraggableDockWidget final : public QDockWidget
{
public:
    explicit DraggableDockWidget(const QString& title, QWidget* parent = nullptr)
        : QDockWidget(title, parent)
    {
        setMouseTracking(true);
    }

protected:
    void enterEvent(QEnterEvent* event) override
    {
        updateDragCursor(event->position().toPoint());
        QDockWidget::enterEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        updateDragCursor(event->position().toPoint());
        QDockWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        setDragCursorVisible(false);
        QDockWidget::leaveEvent(event);
    }

private:
    void updateDragCursor(const QPoint& position)
    {
        const QWidget* const content = widget();
        if (!content) {
            setDragCursorVisible(false);
            return;
        }

        const bool verticalTitleBar =
            features().testFlag(DockWidgetVerticalTitleBar);
        const QRect contentGeometry = content->geometry();
        const QRect titleBar = verticalTitleBar
            ? QRect(0, 0, contentGeometry.left(), height())
            : QRect(0, 0, width(), contentGeometry.top());
        setDragCursorVisible(features().testFlag(DockWidgetMovable)
                             && titleBar.contains(position));
    }

    void setDragCursorVisible(bool visible)
    {
        if (_dragCursorVisible == visible)
            return;

        _dragCursorVisible = visible;
        if (visible)
            setCursor(Qt::SizeAllCursor);
        else
            unsetCursor();
    }

    bool _dragCursorVisible{false};
};

} // namespace

MainWindow::MainWindow(QWidget* parent) : ElaWindow(parent)
{
    qDebug() << "main id: " << QThread::currentThreadId();
    // 主题切换 → 持久化到配置 + 同步 QPalette（必须在 initContent 之前
    // 连接，因为 TerminalView 在构造时也会连接 themeModeChanged，
    // Qt 按连接顺序调用处理函数；若本处理器后连接，TerminalView
    // 的 applyThemeColorScheme() 会先执行，此时 QApplication::palette()
    // 仍为旧主题色，导致滚动条捕获错误的调色板，呈现白色边框）
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode mode) {
        // 配置持久化：仅在用户手动切换时写入；程序化切换（auto 模式 /
        // 跟随系统）不应把配置里的 "auto" 覆盖成具体的 light/dark。
        if (!SettingsPage::s_themeProgrammaticChange) {
            ConfigManager::set("ui.theme",
                               mode == ElaThemeType::Dark ? "dark" : "light");
        }

        // QPalette 同步必须无条件执行（手动与程序化切换都要同步）：终端右侧
        // 的原生 QScrollBar 由 QApplication 调色板绘制，QTermWidget 在
        // setColorScheme() 时执行 _scrollBar->setPalette(QApplication::palette())。
        // 此前该同步与 ConfigManager 写入一起被 s_themeProgrammaticChange 的
        // return 跳过，导致 auto / 跟随系统切换时右侧滚动条残留旧主题色。

        // 同步 QPalette — 必须以干净的 QPalette() 为基准构造，
        // 不能从 app->palette() 复制，否则在上一次已切换为对端
        // 主题的调色板基础上修改，未覆盖的角色（Mid / Dark / Shadow
        // 等，恰为 QScrollBar 所使用）会残留旧主题色，表现为右侧黑边
        // 或白边。
        auto* app = static_cast<QApplication*>(QCoreApplication::instance());
        if (mode == ElaThemeType::Dark) {
            QPalette p;  // 干净基准（平台无关浅色默认值）
            // ElaTheme 深色 WindowBase: #202020, BasicBase: #343434
            p.setColor(QPalette::Window,          QColor(0x20, 0x20, 0x20));
            p.setColor(QPalette::Base,            QColor(0x34, 0x34, 0x34));
            p.setColor(QPalette::AlternateBase,   QColor(0x2A, 0x2A, 0x2A));
            p.setColor(QPalette::WindowText,      QColor(0xF0, 0xF0, 0xF0));
            p.setColor(QPalette::Text,            QColor(0xF0, 0xF0, 0xF0));
            p.setColor(QPalette::Button,          QColor(0x34, 0x34, 0x34));
            p.setColor(QPalette::ButtonText,      QColor(0xF0, 0xF0, 0xF0));
            // 派生色 — QScrollBar 等原生控件用这些角色绘制
            p.setColor(QPalette::Mid,             QColor(0x40, 0x40, 0x40));
            p.setColor(QPalette::Dark,            QColor(0x18, 0x18, 0x18));
            p.setColor(QPalette::Shadow,          QColor(0x10, 0x10, 0x10));
            p.setColor(QPalette::Light,           QColor(0x48, 0x48, 0x48));
            p.setColor(QPalette::Midlight,        QColor(0x3C, 0x3C, 0x3C));
            p.setColor(QPalette::Highlight,       QColor(0x00, 0x78, 0xD4));
            p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
            p.setColor(QPalette::BrightText,      QColor(0xFF, 0x44, 0x44));
            p.setColor(QPalette::Link,            QColor(0x4D, 0xA6, 0xFF));
            app->setPalette(p);
            setPalette(p); // 同步 MainWindow 独立 palette
        } else {
            QPalette p;  // 干净基准（平台无关浅色默认值）
            // ElaTheme 浅色 WindowBase: #ECECEC
            p.setColor(QPalette::Window, QColor(0xEC, 0xEC, 0xEC));
            p.setColor(QPalette::Base,   QColor(0xFF, 0xFF, 0xFF));
            app->setPalette(p);
            setPalette(p);
        }
    });
    initWindow();

    // 语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    // ── 关闭确认对话框 ──────────────────────────────────────────
    auto* closeDialog = new ElaContentDialog(this);
    closeDialog->setWindowTitle(tr("Exit"));
    closeDialog->setLeftButtonText(tr("Cancel"));
    closeDialog->setMiddleButtonText(tr("Minimize"));
    closeDialog->setRightButtonText(tr("Exit"));

    auto* closeCentral = new QWidget(closeDialog);
    auto* closeLayout = new QVBoxLayout(closeCentral);
    closeLayout->setContentsMargins(24, 20, 24, 20);
    auto* closeLabel = new QLabel(tr("Are you sure you want to exit NovaTerm?"), closeCentral);
    closeLabel->setWordWrap(true);
    closeLayout->addWidget(closeLabel);
    closeLayout->addStretch();
    closeDialog->setCentralWidget(closeCentral);
    // setCentralWidget() 内部已调用 adjustSize()，基于当前内容
    // 重新计算正确的 sizeHint，避免 exec() 时触发 QWindowsWindow 几何体警告。

    connect(closeDialog, &ElaContentDialog::rightButtonClicked, this, [closeDialog, this]() {
        closeDialog->done(QDialog::Accepted);
        QTimer::singleShot(0, this, &QWidget::close);
    });
    connect(closeDialog, &ElaContentDialog::middleButtonClicked, this, [=]() {
        closeDialog->done(QDialog::Accepted);
        showMinimized();
    });
    connect(closeDialog, &ElaContentDialog::leftButtonClicked, closeDialog,
            &ElaContentDialog::close);
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

    if (_menuTip) _menuTip->setToolTip(tr("Menu"));
    if (_newSessionTip) _newSessionTip->setToolTip(tr("New session"));
    if (_newSessionButton) {
        _newSessionButton->setAccessibleName(tr("New session"));
        _newSessionButton->setToolTip(tr("New session"));
    }

    // 重新翻译弹出菜单项
    if (_actSession)  _actSession->setText(tr("Session"));
    if (_actSettings) _actSettings->setText(tr("Settings"));
    if (_actAbout)    _actAbout->setText(tr("About"));
    if (_localSessionAction)  _localSessionAction->setText(tr("Local"));
    if (_sshSessionAction)    _sshSessionAction->setText(tr("SSH"));
    if (_serialSessionAction) _serialSessionAction->setText(tr("Serial"));
    if (_telnetSessionAction) _telnetSessionAction->setText(tr("Telnet"));
    if (_sessionDock) _sessionDock->setWindowTitle(tr("Sessions"));
}

void MainWindow::initWindow()
{
    setWindowTitle(tr("NovaTerm"));

    int w = ConfigManager::get<int>("window.width", 1280);
    int h = ConfigManager::get<int>("window.height", 800);
    resize(w, h);

    setAppBarHeight(32);

    // 隐藏用户信息卡片
    setUserInfoCardVisible(false);

    //隐藏导航栏
    setIsNavigationBarEnable(false);

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

    // ═══════════════════════════════════════════════════════════════
    // 菜单图标，放置在 LeftArea 中使其位于 Logo 右侧。
    //   • 悬停  → ElaToolTip "Menu"
    //   • 点击  → 弹出 ElaMenu，包含 会话 / 设置 / 关于
    // ═══════════════════════════════════════════════════════════════
    _menuButton = new ElaIconButton(ElaIconType::Bars, 18, 32, 32, this);
    // ElaToolTip（非原生 QToolTip）：其背景使用主题的 PopupBase/PopupBorder
    // 绘制，并已绑定 eTheme->themeModeChanged，因此自动跟随全局亮色/暗色主题。
    _menuTip = new ElaToolTip(_menuButton);
    _menuTip->setToolTip(tr("Menu"));

    buildMainMenu();
    connect(_menuButton, &QPushButton::clicked, this, [this]() {
        _mainMenu->popup(_menuButton->mapToGlobal(QPoint(0, _menuButton->height() + 2)));
    });

    _newSessionButton = new ElaIconButton(
        ElaIconType::Plus, 14, 32, 32, this);
    _newSessionButton->setAccessibleName(tr("New session"));
    _newSessionButton->setToolTip(tr("New session"));
    _newSessionTip = new ElaToolTip(_newSessionButton);
    _newSessionTip->setToolTip(tr("New session"));
    buildNewSessionMenu();
    _newSessionButton->setMenu(_newSessionMenu);

    QWidget* customWidget = new QWidget(this);
    QHBoxLayout* customLayout = new QHBoxLayout(customWidget);
    // 左边距为零，使 Logo 紧贴窗口左边缘
    // （AppBar 自身左侧布局为空 —— 其所有控件均已隐藏）。
    customLayout->setContentsMargins(0, 0, 0, 0);
    customLayout->setSpacing(6);

    // 应用 Logo，尺寸与 36px 菜单按钮匹配（26px 图标放在 26 宽的槽中，
    // 使其左对齐，并由标题栏布局保持垂直居中）。
    auto* logoLabel = new QLabel(customWidget);
    logoLabel->setPixmap(QIcon(":/icons/app_logo.svg").pixmap(26, 26));
    logoLabel->setFixedSize(26, 32);
    logoLabel->setAlignment(Qt::AlignCenter);
    customLayout->addWidget(logoLabel);

    customLayout->addWidget(_menuButton);
    customLayout->addWidget(_newSessionButton);
    customLayout->addStretch();
    setCustomWidget(ElaAppBarType::LeftArea, customWidget, this, "processHitTest");

    _terminalPage = new TerminalPage(this);

    _sessionDock = new DraggableDockWidget(tr("Sessions"), this);
    _sessionDock->setObjectName(QStringLiteral("sessionDock"));
    _sessionDock->setAllowedAreas(Qt::LeftDockWidgetArea
                                  | Qt::RightDockWidgetArea);
    _sessionDock->setFeatures(QDockWidget::DockWidgetMovable);

    _sessionPanel = new SessionPanel(_sessionDock);
    _sessionPanel->setCursor(Qt::ArrowCursor);
    _sessionDock->setWidget(_sessionPanel);
    addDockWidget(Qt::LeftDockWidgetArea, _sessionDock);
    _sessionPanel->setDockArea(Qt::LeftDockWidgetArea);
    resizeDocks({_sessionDock}, {280}, Qt::Horizontal);

    connect(_sessionDock, &QDockWidget::dockLocationChanged,
            _sessionPanel, &SessionPanel::setDockArea);
    connect(_sessionPanel, &SessionPanel::panelWidthChangeRequested, this,
            [this](int width) {
        resizeDocks({_sessionDock}, {width}, Qt::Horizontal);
    });
    connect(_sessionPanel, &SessionPanel::editSessionRequested, this,
            &MainWindow::editSession);
    connect(_sessionPanel, &SessionPanel::localReconnectRequested, this,
            [this](TerminalView::LocalShellType type) {
        _terminalPage->addTerminalTab(tr("Terminal"), type);
    });
    connect(_sessionPanel, &SessionPanel::serialReconnectRequested, this,
            [this](const SerialConfig& config) {
        _terminalPage->addSerialTerminalTab(config);
    });
    connect(_sessionPanel, &SessionPanel::sshReconnectRequested, this,
            [this](const SshConfig& config) {
        _terminalPage->addSshTerminalTab(config);
    });
    connect(_sessionPanel, &SessionPanel::reconnectUnavailable, this,
            [this](const QString& message) {
        QMessageBox::warning(this, tr("Reconnect session"), message);
    });

    // 仅注册终端（会话）页面；导航通过标题栏菜单进行，
    // 因此左侧导航栏保持隐藏。
    addPageNode(tr("Terminal"), _terminalPage, ElaIconType::Terminal);
}

void MainWindow::buildMainMenu()
{
    _mainMenu = new ElaMenu(this);
    _mainMenu->setMenuItemHeight(27);

    // ── 会话：打开会话/连接选择器 ──
    _actSession = _mainMenu->addElaIconAction(ElaIconType::Terminal, tr("Session"));
    connect(_actSession, &QAction::triggered, this,
            [this]() { showSessionDialog(); });

    // ── 设置：ElaDialog 内嵌现有 SettingsPage ──
    _actSettings = _mainMenu->addElaIconAction(ElaIconType::GearComplex, tr("Settings"));
    connect(_actSettings, &QAction::triggered, this, &MainWindow::showSettingsDialog);

    // ── 关于：模态对话框（行为不变）──
    _actAbout = _mainMenu->addElaIconAction(ElaIconType::CircleInfo, tr("About"));
    connect(_actAbout, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::buildNewSessionMenu()
{
    _newSessionMenu = new ElaMenu(_newSessionButton);
    _newSessionMenu->setMenuItemHeight(32);
    _localSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::Terminal, tr("Local"));
    _sshSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::NetworkWired, tr("SSH"));
    _serialSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::UsbDrive, tr("Serial"));
    _telnetSessionAction = _newSessionMenu->addElaIconAction(
        ElaIconType::Globe, tr("Telnet"));

    connect(_localSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::LocalShell); });
    connect(_sshSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::Ssh); });
    connect(_serialSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::Serial); });
    connect(_telnetSessionAction, &QAction::triggered, this,
            [this]() { showSessionDialog(TransportKind::Telnet); });
}


bool MainWindow::processHitTest()
{
    // 自定义区域中除菜单按钮外均可拖动。
    if (!_menuButton)
        return true;
    return !ElaApplication::containsCursorToItem(_menuButton)
        && (!_newSessionButton
            || !ElaApplication::containsCursorToItem(_newSessionButton));
}

// ═══════════════════════════════════════════════════════════════════
//  会话选择器 — 与 Settings 同模式的 ElaDialog + ElaTabWidget
// ═══════════════════════════════════════════════════════════════════

void MainWindow::showSessionDialog()
{
    showSessionDialog(TransportKind::LocalShell);
}

void MainWindow::showSessionDialog(TransportKind initialKind)
{
    runSessionDialog(initialKind, std::nullopt, std::nullopt, {});
}

void MainWindow::editSession(const SessionId& id,
                             const RuntimeConfig& runtime,
                             const QByteArray& secret)
{
    runSessionDialog(runtime.transportKind, id, runtime, secret);
}

void MainWindow::runSessionDialog(
    TransportKind initialKind,
    const std::optional<SessionId>& editingSessionId,
    const std::optional<RuntimeConfig>& initialConfig,
    const QByteArray& secret)
{
    if (!_sessionDialog) {
        _sessionDialog = new ElaDialog(this);
        _sessionDialog->setWindowTitle(tr("Session"));
        _sessionDialog->resize(1000, 600);
        _sessionDialog->setWindowModality(Qt::ApplicationModal);
        _sessionDialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
        _sessionDialog->setAppBarHeight(30);

        // SessionPage 属于本次对话框调用。对话框在 exec() 返回后、终端/RHI
        // 控件创建前销毁，二者的字体与图形资源清理不会重叠。
        auto* sessionPage = new SessionPage(_sessionDialog);
        sessionPage->setTitleVisible(false);
        sessionPage->selectTransport(initialKind);
        if (initialConfig)
            sessionPage->applyRuntimeConfig(*initialConfig, secret);

        connect(sessionPage, &SessionPage::localSessionRequested, this,
                [this](TerminalView::LocalShellType type,
                       const QString& label) {
            qDebug() << "localSessionRequested, type ="
                     << (type == TerminalView::LocalShellType::PowerShell ? "PowerShell" : "cmd/Clink");
            // 在构造 QRhiWidget 前结束模态事件循环，避免顶层窗口切换到
            // RHI 合成时重入字体布局。
            _pendingLocalSession = LocalSessionParameters{type, label};
            _sessionDialog->accept();
        });
        connect(sessionPage, &SessionPage::serialSessionRequested, this,
                [this](const SerialConfig& config) {
            _pendingSerialSession = config;
            _sessionDialog->accept();
        });
        connect(sessionPage, &SessionPage::sshSessionRequested, this,
                [this](const SshConfig& config) {
            _pendingSshSession = config;
            _sessionDialog->accept();
        });
        connect(sessionPage, &SessionPage::dialogRejected,
                _sessionDialog, &QDialog::reject);

        auto* mainLayout = new QVBoxLayout(_sessionDialog);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->addWidget(sessionPage);
    }

    _pendingLocalSession.reset();
    _pendingSerialSession.reset();
    _pendingSshSession.reset();
    const int result = _sessionDialog->exec();
    // ElaDialog 及其自定义控件在 accept() 后复用不可靠：后续 QDialog::exec()
    // 在递归查找默认按钮时可能遇到残留项。在终端/RHI 初始化前同步销毁已隐藏
    // 的对话框。
    delete _sessionDialog;
    _sessionDialog = nullptr;

    if (result == QDialog::Accepted && _pendingLocalSession) {
        const LocalSessionParameters parameters = *_pendingLocalSession;
        _pendingLocalSession.reset();
        if (editingSessionId) {
            _sessionPanel->updateLocal(*editingSessionId, parameters.type,
                                       parameters.label);
        } else {
            _sessionPanel->recordLocal(parameters.type, parameters.label);
            const QString title = parameters.label.isEmpty()
                ? tr("Terminal") : parameters.label;
            _terminalPage->addTerminalTab(title, parameters.type);
        }
    } else if (result == QDialog::Accepted && _pendingSerialSession) {
        const SerialConfig config = *_pendingSerialSession;
        _pendingSerialSession.reset();
        if (editingSessionId)
            _sessionPanel->updateSerial(*editingSessionId, config);
        else {
            _sessionPanel->recordSerial(config);
            _terminalPage->addSerialTerminalTab(config);
        }
    } else if (result == QDialog::Accepted && _pendingSshSession) {
        const SshConfig config = *_pendingSshSession;
        _pendingSshSession.reset();
        if (editingSessionId)
            _sessionPanel->updateSsh(*editingSessionId, config);
        else {
            _sessionPanel->recordSsh(config);
            _terminalPage->addSshTerminalTab(config);
        }
    }
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

    // ── 使用 AboutPage 构建关于内容 ──
    auto* aboutPage = new AboutPage(dialog);
    aboutPage->setTitleVisible(false);
    dialog->setCentralWidget(aboutPage);

    // ── 语言切换支持：仅更新对话框外框，内容由 AboutPage 自行管理 ──
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            dialog, [dialog]() {
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
