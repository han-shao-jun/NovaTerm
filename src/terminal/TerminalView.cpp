#include "TerminalView.h"
#include "transport/ITransport.h"

#include "qtermwidget.h"
#include <QVBoxLayout>
#include "ElaMenu.h"
#include <QDebug>

TerminalView::TerminalView(QWidget* parent)
    : QWidget(parent)
{
    // startnow=0：电传模式 — 由我们控制数据流
    // （同时用于 ITransport 桥接和本地 KPty）
    _terminal = new QTermWidget(0, this);
    _terminal->startTerminalTeletype();

    _terminal->setColorScheme("system");
    _terminal->setScrollBarPosition(QTermWidgetInterface::ScrollBarRight);
    _terminal->setTerminalFont(QFont("Cascadia Code, Consolas, monospace", 12));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_terminal);

    setFocusProxy(_terminal);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &TerminalView::setupContextMenu);
}

TerminalView::~TerminalView()
{
    stopLocalShell();
    detachTransport();
}

// ═══════════════════════════════════════════════════════════════════
//  本地终端模式 — QTermWidget 内置 KPty
//
//  QTermWidget 内部通过 KPty 直接对接：
//    Windows 10+：ConPTY (CreatePseudoConsole)
//    Unix：       pty (posix_openpt/forkpty)
//
//  数据流：键盘 → KPty → shell 进程 stdin
//         shell stdout → KPty → QTermWidget VT 解析 → 渲染
//  ⚠ 完全不经过 ITransport
// ═══════════════════════════════════════════════════════════════════

void TerminalView::startLocalShell()
{
    stopLocalShell();
    detachTransport();

    // QTermWidget::startShellProgram() 内部使用 KPty：
    //   Windows：KPty → ConPTY → cmd.exe / pwsh.exe
    //   Unix：   KPty → pty → $SHELL
    _terminal->startShellProgram();
    int pid = _terminal->getShellPID();
    if (pid <= 0) {
        qWarning() << "TerminalView: QTermWidget::startShellProgram() 失败";
        emit shellFinished();
        return;
    }

    _isLocalShell = true;

    connect(_terminal, &QTermWidget::finished,
            this, &TerminalView::onLocalShellFinished);

    // 转发标题变更（shell 可能通过转义序列设置终端标题）
    connect(_terminal, &QTermWidget::titleChanged,
            this, [this]() {
        emit titleChanged(_terminal->title());
    });
}

void TerminalView::stopLocalShell()
{
    if (_isLocalShell) {
        disconnect(_terminal, &QTermWidget::finished,
                   this, &TerminalView::onLocalShellFinished);
        _isLocalShell = false;
    }
}

void TerminalView::onLocalShellFinished()
{
    _isLocalShell = false;
    emit shellFinished();
}

// ═══════════════════════════════════════════════════════════════════
//  远程终端模式 — ITransport 数据桥接
//
//  数据流：键盘 → QTermWidget::sendData 信号
//                 → ITransport::write(bytes)
//                 → SshTransport::ssh_channel_write / SerialTransport::write / ...
//
//         ITransport::readyRead 信号
//                 → QTermWidget::receiveData
//                 → VT 解析 → ScreenBuffer → 渲染
// ═══════════════════════════════════════════════════════════════════

void TerminalView::attachTransport(ITransport* transport)
{
    detachTransport();
    stopLocalShell();
    _transport = transport;

    connect(_transport, &ITransport::readyRead,
            this, &TerminalView::onTransportReadyRead);
    connect(_transport, &ITransport::disconnected,
            this, &TerminalView::onTransportDisconnected);

    // 转发终端键盘输入 -> transport
    connect(_terminal, &QTermWidget::sendData,
            this, [this](const char* data, int len) {
        if (_transport && _transport->isConnected())
            _transport->write(QByteArray(data, len));
    });

    // 转发标题变更
    connect(_terminal, &QTermWidget::titleChanged,
            this, [this]() {
        emit titleChanged(_terminal->title());
    });

    // 转发活动信号
    connect(_terminal, &QTermWidget::activity,
            this, &TerminalView::activityDetected);
}

void TerminalView::detachTransport()
{
    if (_transport) {
        disconnect(_transport, nullptr, this, nullptr);
        _transport = nullptr;
    }
}

void TerminalView::onTransportReadyRead(const QByteArray& data)
{
    if (_terminal)
        _terminal->receiveData(data.constData(), data.size());
}

void TerminalView::onTransportDisconnected()
{
    if (_terminal) {
        const char* msg = "\r\n[已断开连接]\r\n";
        _terminal->receiveData(msg, static_cast<int>(strlen(msg)));
    }
}

// ═══════════════════════════════════════════════════════════════════
//  右键菜单
// ═══════════════════════════════════════════════════════════════════

void TerminalView::setupContextMenu(const QPoint& pos)
{
    auto* menu = new ElaMenu(this);
    menu->setMenuItemHeight(27);

    connect(menu->addElaIconAction(ElaIconType::Copy, tr("Copy")),
            &QAction::triggered, _terminal, &QTermWidget::copyClipboard);

    connect(menu->addElaIconAction(ElaIconType::Paste, tr("Paste")),
            &QAction::triggered, _terminal, &QTermWidget::pasteClipboard);

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassPlus, tr("Zoom In")),
            &QAction::triggered, _terminal, &QTermWidget::zoomIn);

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassMinus, tr("Zoom Out")),
            &QAction::triggered, _terminal, &QTermWidget::zoomOut);

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::Broom, tr("Clear Scrollback")),
            &QAction::triggered, _terminal, &QTermWidget::clear);

    menu->popup(mapToGlobal(pos));
}
