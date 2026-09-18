/**
 * @file   TerminalPage.cpp
 * @brief  终端页面实现：标签页管理与终端视图生命周期。
 *
 * 每个标签页内含一个 TerminalView，支持本地/串口/SSH/Telnet 创建方式。
 * 标签上下文菜单由 TerminalTabWidget 管理，页面只负责把标签索引解析为终端视图。
 */
#include "TerminalPage.h"
#include "ui/app/Application.h"
#include "mcp/McpService.h"
#include "ui/terminal/TerminalView.h"
#include "ui/widgets/TerminalTabWidget.h"
#include "renderer/TerminalRenderer.h"
#include "service/LanguageManager.h"
#include "session/SerialHighlightRules.h"
#include "session/TerminalSession.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include "transport/TelnetTransport.h"
#include <QPointer>
#include <QVBoxLayout>

#include <utility>

namespace {

RuntimeConfig localRuntime(TerminalView::LocalShellType type,
                           const QString& wslDistribution,
                           const QString& label,
                           const QString& workingDirectory)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.title = label.trimmed();
    runtime.transport = {
        {QStringLiteral("shellType"), static_cast<int>(type)},
        {QStringLiteral("wslDistribution"), wslDistribution.trimmed()},
        {QStringLiteral("workingDirectory"), workingDirectory.trimmed()},
        {QStringLiteral("label"), label.trimmed()}};
    return runtime;
}

RuntimeConfig serialRuntime(const SerialConfig& config)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.title = config.label.trimmed();
    runtime.transport = {
        {QStringLiteral("portName"), config.portName},
        {QStringLiteral("baudRate"), config.baudRate},
        {QStringLiteral("dataBits"), static_cast<int>(config.dataBits)},
        {QStringLiteral("parity"), static_cast<int>(config.parity)},
        {QStringLiteral("stopBits"), static_cast<int>(config.stopBits)},
        {QStringLiteral("flowControl"), static_cast<int>(config.flowControl)},
        {QStringLiteral("reconnectSeconds"), config.reconnectSeconds},
        {QStringLiteral("label"), config.label}};
    return runtime;
}

RuntimeConfig sshRuntime(const SshConfig& config)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    runtime.title = config.label.trimmed();
    runtime.transport = {
        {QStringLiteral("host"), config.host},
        {QStringLiteral("username"), config.username},
        {QStringLiteral("port"), config.port},
        {QStringLiteral("authMethod"), config.authMethod},
        {QStringLiteral("privateKeyPath"), config.privateKeyPath},
        {QStringLiteral("terminalType"), config.terminalType},
        {QStringLiteral("keepAliveSeconds"), config.keepAliveSeconds},
        {QStringLiteral("label"), config.label}};
    return runtime;
}

RuntimeConfig telnetRuntime(const TelnetConfig& config)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Telnet;
    runtime.title = config.label.trimmed();
    runtime.transport = {
        {QStringLiteral("host"), config.host},
        {QStringLiteral("port"), config.port},
        {QStringLiteral("terminalType"), config.terminalType},
        {QStringLiteral("naws"), config.naws},
        {QStringLiteral("binaryMode"), config.binaryMode},
        {QStringLiteral("keepAliveSeconds"), config.keepAliveSeconds},
        {QStringLiteral("label"), config.label}};
    return runtime;
}

QByteArray sshSecret(const SshConfig& config)
{
    return config.authMethod == QStringLiteral("password")
        ? config.password.toUtf8() : config.keyPassphrase.toUtf8();
}

} // namespace

TerminalPage::TerminalPage(QWidget* parent) : QWidget(parent)
{
    _tabWidget = new TerminalTabWidget(this);
    _tabWidget->setTabPosition(QTabWidget::North);
    _tabWidget->setIndicatorPosition(ElaTabBarType::Bottom);
    _tabWidget->setTabsClosable(true);
    _tabWidget->setMovable(true);
    _tabWidget->setIsTabTransparent(true);

    // 中心就是纯粹的 TerminalTabWidget：零边距、零间距的布局让其完全铺满，
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
    connect(_tabWidget, &TerminalTabWidget::editSessionRequested,
            this, [this](int index) {
        QWidget* const tab = _tabWidget->widget(index);
        auto* terminalView = qobject_cast<TerminalView*>(tab);
        if (!terminalView && tab)
            terminalView = tab->findChild<TerminalView*>();
        const auto snapshot = _sessionEditSnapshots.constFind(terminalView);
        if (snapshot != _sessionEditSnapshots.cend()) {
            emit editSessionRequested(terminalView, snapshot->runtime,
                                      snapshot->secret);
        }
    });
    connect(_tabWidget, &TerminalTabWidget::disconnectRequested,
            this, [this](QWidget* page) {
        auto* const terminalView = qobject_cast<TerminalView*>(page);
        if (!terminalView || !terminalView->session())
            return;
        _tabWidget->setTabConnectionAction(
            page, TerminalTabWidget::ConnectionAction::Hidden);
        if (!terminalView->session()->disconnectForReconnect())
            updateTerminalTabConnectionAction(terminalView);
    });
    connect(_tabWidget, &TerminalTabWidget::reconnectRequested,
            this, [this](QWidget* page) {
        auto* const terminalView = qobject_cast<TerminalView*>(page);
        if (!terminalView || !terminalView->session())
            return;
        _tabWidget->setTabConnectionAction(
            page, TerminalTabWidget::ConnectionAction::Hidden);
        if (!terminalView->session()->reconnect())
            updateTerminalTabConnectionAction(terminalView);
    });

    // 窗口标题只在此处经 tr() 设置，构造期不再单独赋值，避免两份文案漂移。
    retranslateUi();
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
    _sessionEditSnapshots.clear();
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
                                           TerminalView::LocalShellType type,
                                           const QString& wslDistribution,
                                           const QString& workingDirectory)
{
    auto* terminalView = new TerminalView(_tabWidget);
    registerTerminalView(terminalView);
    const QString tabTitle = title.isEmpty()
        ? tr("Terminal %1").arg(_terminalViews.size()) : title;
    _tabWidget->setCurrentIndex(_tabWidget->addTab(terminalView, tabTitle));
    static_cast<void>(replaceTerminalTab(
        terminalView, type, wslDistribution, tabTitle, workingDirectory));
    return terminalView;
}

TerminalView* TerminalPage::addSerialTerminalTab(const SerialConfig& config)
{
    auto* terminalView = new TerminalView(_tabWidget);
    registerTerminalView(terminalView);
    const QString title = config.label.isEmpty() ? config.portName : config.label;
    _tabWidget->setCurrentIndex(_tabWidget->addTab(terminalView, title));
    static_cast<void>(replaceTerminalTab(terminalView, config));
    return terminalView;
}

TerminalView* TerminalPage::addTelnetTerminalTab(const TelnetConfig& config)
{
    auto* terminalView = new TerminalView(_tabWidget);
    registerTerminalView(terminalView);
    const QString title = config.label.isEmpty()
        ? QStringLiteral("%1:%2").arg(config.host).arg(config.port)
        : config.label;
    _tabWidget->setCurrentIndex(_tabWidget->addTab(terminalView, title));
    static_cast<void>(replaceTerminalTab(terminalView, config));
    return terminalView;
}

TerminalView* TerminalPage::addSshTerminalTab(const SshConfig& config)
{
    auto* terminalView = new TerminalView(_tabWidget);
    registerTerminalView(terminalView);
    const QString title = config.label.isEmpty()
        ? QStringLiteral("%1@%2").arg(config.username, config.host)
        : config.label;
    _tabWidget->setCurrentIndex(_tabWidget->addTab(terminalView, title));
    static_cast<void>(replaceTerminalTab(terminalView, config));
    return terminalView;
}

void TerminalPage::registerTerminalView(TerminalView* terminalView)
{
    _terminalViews.append(terminalView);
    if (auto* service = Application::instance().mcpService())
        service->directory().add(terminalView->session());

    // 关闭标签页时 ElaTabWidget 会 deleteLater() 对应视图；同步清除两份索引，
    // 避免 currentTerminal() 回退或右键编辑读取悬垂指针。
    connect(terminalView, &QObject::destroyed, this, [this, terminalView]() {
        _terminalViews.removeAll(terminalView);
        _sessionEditSnapshots.remove(terminalView);
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
    TerminalSession* const session = terminalView->session();
    connect(session, &TerminalSession::stateChanged, this,
            [this, terminalView](SessionState) {
        updateTerminalTabConnectionAction(terminalView);
    });
    connect(session, &TerminalSession::connected, this,
            [this, terminalView](ITransport*) {
        updateTerminalTabConnectionAction(terminalView);
    });
    connect(session, &TerminalSession::disconnected, this,
            [this, terminalView](ITransport*) {
        updateTerminalTabConnectionAction(terminalView);
    });
}

void TerminalPage::updateTerminalTabConnectionAction(
    TerminalView* terminalView)
{
    if (!terminalView || _tabWidget->indexOf(terminalView) < 0)
        return;
    TerminalSession* const session = terminalView->session();
    ITransport* const transport = session ? session->transport() : nullptr;
    auto action = TerminalTabWidget::ConnectionAction::Hidden;
    if (session && transport && session->state() == SessionState::Running
        && transport->isConnected() && session->canReconnect()) {
        action = TerminalTabWidget::ConnectionAction::Disconnect;
    } else if (session && transport && session->state() == SessionState::Failed
               && !transport->isConnected() && session->canReconnect()) {
        action = TerminalTabWidget::ConnectionAction::Reconnect;
    }
    _tabWidget->setTabConnectionAction(terminalView, action);
}

void TerminalPage::restartTerminalWhenClosed(
    TerminalView* terminalView, std::function<void()> restart)
{
    if (!terminalView || !terminalView->session())
        return;

    const QPointer<TerminalView> terminalGuard(terminalView);
    TerminalSession* const session = terminalView->session();
    session->close(CloseMode::Graceful);
    if (session->state() == SessionState::Closed) {
        restart();
        return;
    }

    // ConPTY 等后端异步关闭。等会话真正 Closed 后再换绑，避免在 Closing 状态
    // 调用 start() 被状态机拒绝；标签与 Core 在等待期间继续存活。
    connect(session, &TerminalSession::stateChanged, this,
            [terminalGuard, restart = std::move(restart)](
                SessionState state) mutable {
        if (state == SessionState::Closed && terminalGuard)
            restart();
    }, Qt::SingleShotConnection);
}

bool TerminalPage::replaceTerminalTab(TerminalView* terminalView,
                                      TerminalView::LocalShellType type,
                                      const QString& wslDistribution,
                                      const QString& label,
                                      const QString& workingDirectory)
{
    const int index = _tabWidget->indexOf(terminalView);
    if (index < 0)
        return false;

    const QString title = label.trimmed().isEmpty()
        ? tr("Terminal %1").arg(index + 1) : label.trimmed();
    _tabWidget->setTabText(index, title);
    terminalView->setProperty("novatermSessionLabel", title);
    terminalView->setProperty("novatermSshSession", false);
    terminalView->renderer()->setHighlightRules({});
    _sessionEditSnapshots.insert(
        terminalView,
        SessionEditSnapshot{localRuntime(type, wslDistribution, title, workingDirectory), {}});

    const QPointer<TerminalView> terminalGuard(terminalView);
    restartTerminalWhenClosed(terminalView,
        [this, terminalGuard, type, wslDistribution, workingDirectory]() {
        if (!terminalGuard)
            return;
        terminalGuard->startLocalShell(type, wslDistribution, workingDirectory);
        if (ITransport* const transport = terminalGuard->transport()) {
            if (transport->isConnected()) {
                emit localSessionConnected(type);
            } else {
                connect(transport, &ITransport::connected, this,
                        [this, type]() { emit localSessionConnected(type); },
                        Qt::SingleShotConnection);
            }
        }
    });
    emitCurrentSessionContext();
    return true;
}

bool TerminalPage::replaceTerminalTab(TerminalView* terminalView,
                                      const SerialConfig& config)
{
    const int index = _tabWidget->indexOf(terminalView);
    if (index < 0)
        return false;

    const QString title = config.label.trimmed().isEmpty()
        ? config.portName : config.label.trimmed();
    _tabWidget->setTabText(index, title);
    terminalView->setProperty("novatermSessionLabel", title);
    terminalView->setProperty("novatermSshSession", false);
    terminalView->renderer()->setHighlightRules(
        NovaTerm::serialLogHighlightRules());
    _sessionEditSnapshots.insert(
        terminalView, SessionEditSnapshot{serialRuntime(config), {}});

    const QPointer<TerminalView> terminalGuard(terminalView);
    restartTerminalWhenClosed(terminalView, [this, terminalGuard, config]() {
        if (!terminalGuard)
            return;
        auto* transport = new SerialTransport(config, terminalGuard);
        terminalGuard->attachTransport(transport);
        terminalGuard->session()->setSerialReconnectInterval(config.reconnectSeconds);
        connect(transport, &ITransport::connected, this,
                [this, config]() { emit serialSessionConnected(config); },
                Qt::SingleShotConnection);
        static_cast<void>(terminalGuard->session()->start());
    });
    emitCurrentSessionContext();
    return true;
}

bool TerminalPage::replaceTerminalTab(TerminalView* terminalView,
                                      const TelnetConfig& config)
{
    const int index = _tabWidget->indexOf(terminalView);
    if (index < 0)
        return false;

    const QString title = config.label.trimmed().isEmpty()
        ? QStringLiteral("%1:%2").arg(config.host).arg(config.port)
        : config.label.trimmed();
    _tabWidget->setTabText(index, title);
    terminalView->setProperty("novatermSessionLabel", title);
    terminalView->setProperty("novatermSshSession", false);
    terminalView->renderer()->setHighlightRules({});
    _sessionEditSnapshots.insert(
        terminalView, SessionEditSnapshot{telnetRuntime(config), {}});

    const QPointer<TerminalView> terminalGuard(terminalView);
    restartTerminalWhenClosed(terminalView, [this, terminalGuard, config]() {
        if (!terminalGuard)
            return;
        auto* transport = new TelnetTransport(config, terminalGuard);
        terminalGuard->attachTransport(transport);
        connect(transport, &ITransport::connected, this,
                [this, config]() { emit telnetSessionConnected(config); },
                Qt::SingleShotConnection);
        static_cast<void>(terminalGuard->session()->start());
    });
    emitCurrentSessionContext();
    return true;
}

bool TerminalPage::replaceTerminalTab(TerminalView* terminalView,
                                      const SshConfig& config)
{
    const int index = _tabWidget->indexOf(terminalView);
    if (index < 0)
        return false;

    const QString title = config.label.isEmpty()
        ? QStringLiteral("%1@%2").arg(config.username, config.host)
        : config.label.trimmed();
    _tabWidget->setTabText(index, title);
    // 标记 SSH 标签；最终是否可用仍由传输层连接状态决定。
    terminalView->setProperty("novatermSessionLabel", title);
    terminalView->setProperty("novatermSshSession", true);
    terminalView->renderer()->setHighlightRules({});
    _sessionEditSnapshots.insert(
        terminalView,
        SessionEditSnapshot{sshRuntime(config), sshSecret(config)});

    const QPointer<TerminalView> terminalGuard(terminalView);
    restartTerminalWhenClosed(terminalView, [this, terminalGuard, config]() {
        if (!terminalGuard)
            return;
        auto* transport = new SshTransport(config, terminalGuard);
        terminalGuard->attachTransport(transport);
        // 连接建立或断开都需要刷新工具面板，防止保留失效的远端状态。
        connect(transport, &ITransport::connected, this,
                &TerminalPage::emitCurrentSessionContext);
        connect(transport, &ITransport::disconnected, this,
                &TerminalPage::emitCurrentSessionContext);
        connect(transport, &ITransport::connected, this,
                [this, config]() { emit sshSessionConnected(config); },
                Qt::SingleShotConnection);
        static_cast<void>(terminalGuard->session()->start());
    });
    emitCurrentSessionContext();
    return true;
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
