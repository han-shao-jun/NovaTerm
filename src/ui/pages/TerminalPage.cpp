/**
 * @file   TerminalPage.cpp
 * @brief  终端页面实现：标签页管理与终端视图生命周期。
 *
 * 每个标签页内含一个 TerminalView，支持本地/串口/SSH 三种创建方式。
 * 新建会话按钮弹出菜单选择传输类型，信号转发给 MainWindow。
 */
#include "TerminalPage.h"
#include "ElaTabWidget.h"
#include "ui/terminal/TerminalView.h"
#include "renderer/TerminalRenderer.h"
#include "service/LanguageManager.h"
#include "session/SerialHighlightRules.h"
#include "session/TerminalSession.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include <QVBoxLayout>

#include <utility>

TerminalPage::TerminalPage(QWidget* parent) : QWidget(parent)
{
    setWindowTitle(tr("Terminal"));

    _tabWidget = new ElaTabWidget(this);
    _tabWidget->setTabPosition(QTabWidget::North);
    _tabWidget->setIndicatorPosition(ElaTabBarType::Bottom);
    _tabWidget->setTabsClosable(true);
    _tabWidget->setMovable(true);
    _tabWidget->setIsTabTransparent(true);

    // 中心就是纯粹的 ElaTabWidget：零边距、零间距的布局让其完全铺满，
    // 四周不留空隙。标题栏由 ElaWindow 的 AppBar 单独绘制，不受影响。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_tabWidget);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    // 标签切换后通知依赖当前连接的 SFTP/资源监视面板刷新上下文。
    connect(_tabWidget, &QTabWidget::currentChanged, this,
            [this](int) { emitCurrentSessionContext(); });
}

void TerminalPage::retranslateUi()
{
    setWindowTitle(tr("Terminal"));
}

TerminalPage::~TerminalPage()
{
    // C++ 成员在 QWidget 析构函数删除子控件之前销毁。下面的 destroyed 处理器
    // 访问 _terminalViews，因此不能在本析构体返回后、列表生命周期结束时仍保持
    // 连接。已关闭的标签页已自行移除；剩余条目此刻仍是存活的子控件。
    for (TerminalView* terminalView : std::as_const(_terminalViews)) {
        if (terminalView)
            disconnect(terminalView, nullptr, this, nullptr);
    }
    _terminalViews.clear();
}

TerminalView* TerminalPage::currentTerminal() const
{
    QWidget* currentWidget = _tabWidget->currentWidget();
    // 当前标签通常就是 TerminalView；findChild() 不会返回对象自身，
    // 因此先直接转换，再兼容未来可能增加的标签容器控件。
    if (auto* terminalView = qobject_cast<TerminalView*>(currentWidget))
        return terminalView;
    if (currentWidget)
        return currentWidget->findChild<TerminalView*>();
    return _terminalViews.isEmpty() ? nullptr : _terminalViews.first();
}

TerminalView* TerminalPage::currentConnectedSshTerminal() const
{
    TerminalView* const terminalView = currentTerminal();
    if (!terminalView
        || !terminalView->property("novatermSshSession").toBool()) {
        return nullptr;
    }

    ITransport* const transport = terminalView->transport();
    return transport && transport->isConnected() ? terminalView : nullptr;
}

void TerminalPage::pastePathToCurrentSshTerminal(const QString& path)
{
    TerminalView* const terminalView = currentConnectedSshTerminal();
    if (terminalView && !path.isEmpty())
        terminalView->pasteText(path);
}

void TerminalPage::synchronizeCurrentSshTerminalPath(const QString& path)
{
    TerminalView* const terminalView = currentConnectedSshTerminal();
    if (!terminalView || path.isEmpty())
        return;

    // 按远端终端可直接执行的形式发送，不在路径外额外添加引号。
    terminalView->submitText(QStringLiteral("cd %1").arg(path));
}

void TerminalPage::requestCurrentSshTerminalPath()
{
    TerminalView* const terminalView = currentConnectedSshTerminal();
    if (!terminalView) {
        emit currentSshTerminalPathLookupFailed();
        return;
    }
    terminalView->requestWorkingDirectory();
}

void TerminalPage::emitCurrentSessionContext()
{
    TerminalView* const terminalView = currentTerminal();
    if (!terminalView) {
        emit currentSessionContextChanged({}, false);
        emit currentSftpContextChanged({}, nullptr);
        return;
    }

    // 属性仅承担 UI 上下文桥接，不把具体 Transport 类型暴露给工具面板。
    // 只有当前标签是 SSH 且传输层已连接时，远端工具才获得可用上下文。
    const QString label = terminalView
        ->property("novatermSessionLabel").toString();
    const bool isSshSession = terminalView
        ->property("novatermSshSession").toBool();
    const ITransport* const transport = terminalView->transport();
    emit currentSessionContextChanged(
        label, isSshSession && transport && transport->isConnected());
    auto* sshTransport = isSshSession
        ? qobject_cast<SshTransport*>(terminalView->transport()) : nullptr;
    emit currentSftpContextChanged(
        label, sshTransport && sshTransport->isConnected()
            ? sshTransport : nullptr);
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
        // QObject::destroyed 在 QObject 析构函数中发出，此时 TerminalView 子对象
        // 生命周期已结束。保留它在存活时捕获的指针值，而非在那里对 QObject*
        // 做向下转型。
        _terminalViews.removeAll(terminalView);
    });

    QString tabTitle = title.isEmpty() ? tr("Terminal %1").arg(_terminalViews.size()) : title;
    // 为每个标签保存轻量上下文，标签切换时无需反向解析标题或传输类型。
    terminalView->setProperty("novatermSessionLabel", tabTitle);
    terminalView->setProperty("novatermSshSession", false);
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
    // 串口标签不是远端 SSH 上下文，工具面板应保持不可用状态。
    terminalView->setProperty("novatermSessionLabel", title);
    terminalView->setProperty("novatermSshSession", false);
    const int index = _tabWidget->addTab(terminalView, title);
    _tabWidget->setCurrentIndex(index);

    auto* transport = new SerialTransport(config, terminalView);
    terminalView->attachTransport(transport);
    connect(transport, &ITransport::connected, this,
            [this, config]() { emit serialSessionConnected(config); },
            Qt::SingleShotConnection);
    // 必须通过 TerminalSession 启动，才能让 Created → Connecting → Running
    // 状态迁移完整生效，并在断连后进入可按 Enter 重连的 Failed 状态。
    static_cast<void>(terminalView->session()->start());
    return terminalView;
}

TerminalView* TerminalPage::addSshTerminalTab(const SshConfig& config)
{
    auto* terminalView = new TerminalView(_tabWidget);
    _terminalViews.append(terminalView);
    connect(terminalView, &QObject::destroyed, this, [this, terminalView]() {
        _terminalViews.removeAll(terminalView);
    });
    connect(terminalView, &TerminalView::workingDirectoryReported,
            this, [this, terminalView](const QString& path) {
        if (terminalView == currentConnectedSshTerminal())
            emit currentSshTerminalPathResolved(path);
    });
    connect(terminalView, &TerminalView::workingDirectoryRequestFailed,
            this, [this, terminalView]() {
        if (terminalView == currentConnectedSshTerminal())
            emit currentSshTerminalPathLookupFailed();
    });

    const QString title = config.label.isEmpty()
        ? QStringLiteral("%1@%2").arg(config.username, config.host)
        : config.label;
    // 标记 SSH 标签；最终是否可用仍由传输层连接状态决定。
    terminalView->setProperty("novatermSessionLabel", title);
    terminalView->setProperty("novatermSshSession", true);
    const int index = _tabWidget->addTab(terminalView, title);
    _tabWidget->setCurrentIndex(index);

    auto* transport = new SshTransport(config, terminalView);
    terminalView->attachTransport(transport);
    // 连接建立或断开都需要刷新工具面板，防止保留已经失效的远端状态。
    connect(transport, &ITransport::connected, this,
            &TerminalPage::emitCurrentSessionContext);
    connect(transport, &ITransport::disconnected, this,
            &TerminalPage::emitCurrentSessionContext);
    connect(transport, &ITransport::connected, this,
            [this, config]() { emit sshSessionConnected(config); },
            Qt::SingleShotConnection);
    // SSH 与串口统一由会话状态机启动，避免绕过 generation、统计和重连逻辑。
    static_cast<void>(terminalView->session()->start());
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
