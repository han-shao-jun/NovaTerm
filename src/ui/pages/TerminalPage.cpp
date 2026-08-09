#include "TerminalPage.h"
#include "ElaIconButton.h"
#include "ElaMenu.h"
#include "ElaTabWidget.h"
#include "ui/terminal/TerminalView.h"
#include "renderer/TerminalRenderer.h"
#include "service/LanguageManager.h"
#include "session/SerialHighlightRules.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include "ui/widgets/TabActionWidget.h"
#include <QVBoxLayout>

#include <utility>

TerminalPage::TerminalPage(QWidget* parent) : QWidget(parent)
{
    setWindowTitle(tr("Terminal"));

    auto* terminalTabs = new TabActionWidget(this);
    _tabWidget = terminalTabs;
    _tabWidget->setTabPosition(QTabWidget::North);
    _tabWidget->setIndicatorPosition(ElaTabBarType::Bottom);
    _tabWidget->setTabsClosable(true);
    _tabWidget->setMovable(true);
    _tabWidget->setIsTabTransparent(true);

    _newSessionButton = new ElaIconButton(
        ElaIconType::Plus, 14, 32, 32, _tabWidget);
    _newSessionButton->setAccessibleName(tr("New session"));
    _newSessionButton->setToolTip(tr("New session"));

    auto* newSessionMenu = new ElaMenu(_newSessionButton);
    newSessionMenu->setMenuItemHeight(32);
    _localAction = newSessionMenu->addElaIconAction(
        ElaIconType::Terminal, tr("Local"));
    _sshAction = newSessionMenu->addElaIconAction(
        ElaIconType::NetworkWired, tr("SSH"));
    _serialAction = newSessionMenu->addElaIconAction(
        ElaIconType::UsbDrive, tr("Serial"));
    _telnetAction = newSessionMenu->addElaIconAction(
        ElaIconType::Globe, tr("Telnet"));
    _newSessionButton->setMenu(newSessionMenu);
    terminalTabs->setTabBarActionWidget(_newSessionButton);

    connect(_localAction, &QAction::triggered, this, [this]() {
        emit newSessionRequested(TransportKind::LocalShell);
    });
    connect(_sshAction, &QAction::triggered, this, [this]() {
        emit newSessionRequested(TransportKind::Ssh);
    });
    connect(_serialAction, &QAction::triggered, this, [this]() {
        emit newSessionRequested(TransportKind::Serial);
    });
    connect(_telnetAction, &QAction::triggered, this, [this]() {
        emit newSessionRequested(TransportKind::Telnet);
    });

    // 中心就是纯粹的 ElaTabWidget：零边距、零间距的布局让其完全铺满，
    // 四周不留空隙。标题栏由 ElaWindow 的 AppBar 单独绘制，不受影响。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_tabWidget);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void TerminalPage::retranslateUi()
{
    setWindowTitle(tr("Terminal"));
    _newSessionButton->setAccessibleName(tr("New session"));
    _newSessionButton->setToolTip(tr("New session"));
    _sshAction->setText(tr("SSH"));
    _serialAction->setText(tr("Serial"));
    _telnetAction->setText(tr("Telnet"));
    _localAction->setText(tr("Local"));
}

TerminalPage::~TerminalPage()
{
    // C++ members are destroyed before QWidget's destructor deletes child
    // widgets.  The destroyed handlers below access _terminalViews, so they
    // must not remain connected after this destructor body returns and the
    // list's lifetime ends.  Closed tabs have already removed themselves;
    // every remaining entry is still a live child at this point.
    for (TerminalView* terminalView : std::as_const(_terminalViews)) {
        if (terminalView)
            disconnect(terminalView, nullptr, this, nullptr);
    }
    _terminalViews.clear();
}

TerminalView* TerminalPage::currentTerminal() const
{
    QWidget* currentWidget = _tabWidget->currentWidget();
    if (currentWidget)
    {
        return currentWidget->findChild<TerminalView*>();
    }
    return _terminalViews.isEmpty() ? nullptr : _terminalViews.first();
}

TerminalView* TerminalPage::addTerminalTab(const QString& title,
                                           TerminalView::LocalShellType type)
{
    auto* terminalView = new TerminalView(_tabWidget);
    _terminalViews.append(terminalView);

    // 关闭标签页时 ElaTabWidget 会 deleteLater() 掉对应的 TerminalView，
    // 但它不知道我们这份 _terminalViews 列表。若不同步移除，列表里会残留
    // 悬垂指针：currentTerminal() 的回退分支 _terminalViews.first() 以及任何
    // 遍历都会访问已释放对象，是切换主题/新建终端时偶发崩溃的根因。
    // 绑定 destroyed 信号确保无论以何种方式销毁（关闭、拖出、父对象析构）
    // 都能把指针从列表里摘掉。
    connect(terminalView, &QObject::destroyed, this, [this, terminalView]() {
        // QObject::destroyed is emitted from QObject's destructor, after the
        // TerminalView subobject lifetime has ended.  Retain the pointer value
        // captured while it was alive instead of downcasting QObject* there.
        _terminalViews.removeAll(terminalView);
    });

    QString tabTitle = title.isEmpty() ? tr("Terminal %1").arg(_terminalViews.size()) : title;
    int index = _tabWidget->addTab(terminalView, tabTitle);
    _tabWidget->setCurrentIndex(index);

    terminalView->startLocalShell(type);
    if (ITransport* transport = terminalView->transport()) {
        if (transport->isConnected()) {
            emit localSessionConnected(type);
        } else {
            connect(transport, &ITransport::connected, this,
                    [this, type]() { emit localSessionConnected(type); },
                    Qt::SingleShotConnection);
        }
    }
    return terminalView;
}

TerminalView* TerminalPage::addSerialTerminalTab(const SerialConfig& config)
{
    auto* terminalView = new TerminalView(_tabWidget);
    terminalView->renderer()->setHighlightRules(
        NovaTerm::serialLogHighlightRules());
    _terminalViews.append(terminalView);
    connect(terminalView, &QObject::destroyed, this, [this, terminalView]() {
        _terminalViews.removeAll(terminalView);
    });

    const QString title = config.label.isEmpty() ? config.portName : config.label;
    const int index = _tabWidget->addTab(terminalView, title);
    _tabWidget->setCurrentIndex(index);

    auto* transport = new SerialTransport(config, terminalView);
    terminalView->attachTransport(transport);
    connect(transport, &ITransport::connected, this,
            [this, config]() { emit serialSessionConnected(config); },
            Qt::SingleShotConnection);
    transport->connectToHost();
    return terminalView;
}

TerminalView* TerminalPage::addSshTerminalTab(const SshConfig& config)
{
    auto* terminalView = new TerminalView(_tabWidget);
    _terminalViews.append(terminalView);
    connect(terminalView, &QObject::destroyed, this, [this, terminalView]() {
        _terminalViews.removeAll(terminalView);
    });

    const QString title = config.label.isEmpty()
        ? QStringLiteral("%1@%2").arg(config.username, config.host)
        : config.label;
    const int index = _tabWidget->addTab(terminalView, title);
    _tabWidget->setCurrentIndex(index);

    auto* transport = new SshTransport(config, terminalView);
    terminalView->attachTransport(transport);
    connect(transport, &ITransport::connected, this,
            [this, config]() { emit sshSessionConnected(config); },
            Qt::SingleShotConnection);
    transport->connectToHost();
    return terminalView;
}

void TerminalPage::openLocalTerminal()
{
    // QTermWidget 内置 KPty 直接启动 shell → 零额外 Transport 依赖
    // 交互程序 (vim/htop/tmux) 完美工作 — 因为走的是真实的 ConPTY/pty
    TerminalView* terminal = currentTerminal();
    if (terminal)
    {
        terminal->startLocalShell();
    }
}
