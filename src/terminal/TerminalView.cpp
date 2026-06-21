#include "TerminalView.h"
#include "transport/ITransport.h"

#include "qtermwidget.h"
#include <QVBoxLayout>
#include "ElaMenu.h"
#include "ElaTheme.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>

#ifdef _WIN32
#include "WinConPty.h"
#endif

TerminalView::TerminalView(QWidget* parent)
    : QWidget(parent)
{
    // startnow=0：不在构造时自动启动 shell，由调用方决定本地/远程模式
    _terminal = new QTermWidget(0, this);

    applyThemeColorScheme();
    _terminal->setScrollBarPosition(QTermWidgetInterface::ScrollBarRight);

    // 终端必须使用等宽字体：QTermWidget 假定字符单元格等宽，否则光标
    // 位置会与字符错位。QFont 单参构造不支持 "A, B, C" 形式的回退列表
    // （会被当成一个不存在的字体名），必须用 setFamilies() 提供回退列表，
    // 并显式声明 FixedPitch，确保系统回退时仍挑选等宽字体。
    QFont terminalFont;
    terminalFont.setFamilies({"Cascadia Code", "Consolas", "DejaVu Sans Mono", "monospace"});
    terminalFont.setStyleHint(QFont::Monospace);
    terminalFont.setFixedPitch(true);
    terminalFont.setPointSize(12);
    _terminal->setTerminalFont(terminalFont);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_terminal);

    setFocusProxy(_terminal);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &TerminalView::setupContextMenu);

    // 跟随 ElaTheme 明暗切换同步终端配色方案
    connect(eTheme, &ElaTheme::themeModeChanged,
            this, &TerminalView::applyThemeColorScheme);
}

TerminalView::~TerminalView()
{
    stopLocalShell();
    detachTransport();
}

// ═══════════════════════════════════════════════════════════════════
//  本地终端模式
//
//  Windows：使用 WinConPty（CreatePseudoConsole API），因为 QTermWidget
//           内置的 KPty 在 Windows 上是空桩（pty_win32_stubs.cpp）。
//           数据通过 QTermWidget 的电传模式桥接。
//  Unix：   使用 QTermWidget 内置 KPty（posix_openpt/forkpty）。
//
//  数据流：
//    Windows：键盘 → Emulation → sendData 信号 → WinConPty::write
//             WinConPty 读线程 → QTermWidget::receiveData → 渲染
//    Unix：   键盘 → KPty → shell 进程 stdin
//             shell stdout → KPty → VT 解析 → 渲染
// ═══════════════════════════════════════════════════════════════════

void TerminalView::startLocalShell()
{
    stopLocalShell();
    detachTransport();

#ifdef Q_OS_WIN
    // ── Windows：WinConPty（CreatePseudoConsole）────────────────
    _winPty = new WinConPty(this);

    // WinConPty 输出 → QTermWidget 渲染
    connect(_winPty, &WinConPty::receivedData, _terminal,
            [this](const char* data, int len) {
        _terminal->receiveData(data, len);
    });

    // WinConPty 退出
    connect(_winPty, &WinConPty::finished, this, [this](int) {
        onLocalShellFinished();
    });

    // 切换到电传模式：将 Emulation::sendData 路由到 QTermWidget::sendData
    // 信号，而非内部的 KPty（Windows 上是空桩）
    _terminal->startTerminalTeletype();

    // 键盘输入 → WinConPty → ConPTY → shell stdin
    connect(_terminal, &QTermWidget::sendData, _winPty,
            [this](const char* data, int len) {
        if (_winPty && _winPty->isRunning())
            _winPty->writeData(data, len);
    });

    QString comSpec = QString::fromLocal8Bit(qgetenv("ComSpec"));
    QString shell = comSpec.isEmpty() ? QStringLiteral("powershell.exe") : comSpec;

    if (!_winPty->start(shell)) {
        qWarning() << "TerminalView: WinConPty 启动失败";
        delete _winPty;
        _winPty = nullptr;
        emit shellFinished();
        return;
    }

#else
    // ── Unix：QTermWidget 内置 KPty ─────────────────────────────
    _terminal->startShellProgram();

    int pid = _terminal->getShellPID();
    if (pid <= 0) {
        qWarning() << "TerminalView: QTermWidget::startShellProgram() 失败";
        emit shellFinished();
        return;
    }
#endif

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
    if (!_isLocalShell)
        return;

    disconnect(_terminal, &QTermWidget::finished,
               this, &TerminalView::onLocalShellFinished);
    _isLocalShell = false;

#ifdef _WIN32
    if (_winPty) {
        _winPty->stop();
        delete _winPty;
        _winPty = nullptr;
    }
#endif
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

    // 切换到电传模式：打开空 PTY，将 Emulation::sendData 重新路由到
    // QTermWidget::sendData 信号（而非内部 PTY 进程）。
    _terminal->startTerminalTeletype();

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
//  主题适配 — 终端背景跟随 ElaTheme 明暗模式
// ═══════════════════════════════════════════════════════════════════

void TerminalView::applyThemeColorScheme()
{
    // themeModeChanged 是全局信号：当某个标签页正在 deleteLater() 析构途中
    // （对象尚存但 _terminal 可能已被清理）时切换主题，仍会回调到这里。
    // 防止对空/半析构的终端调用 setColorScheme 造成崩溃。
    if (!_terminal)
        return;

    static const QString kSchemeDir =
        QCoreApplication::applicationDirPath()
        + QStringLiteral("/../../third_party/qtermwidget/lib/color-schemes/");

    bool isDark = (eTheme->getThemeMode() == ElaThemeType::Dark);
    _terminal->setColorScheme(kSchemeDir + (isDark
        ? QStringLiteral("DarkPastels.colorscheme")
        : QStringLiteral("BlackOnWhite.colorscheme")));

    // 同步容器背景色，避免切换主题后边缘/顶部露出旧主题色。
    // setAutoFillBackground 确保样式表环境下的容器也能用调色板填充。
    QColor bg = isDark ? QColor(0x2C, 0x2C, 0x2C) : QColor(0xFD, 0xFD, 0xFD);
    auto* app = static_cast<QApplication*>(QCoreApplication::instance());
    auto applyBg = [bg, app](QWidget* w) {
        w->setAutoFillBackground(true);
        // 以 QApplication::palette() 为基准（已由 MainWindow 的
        // themeModeChanged 处理器同步了所有角色），仅覆盖背景色。
        QPalette p = app->palette();
        p.setColor(w->backgroundRole(), bg);
        w->setPalette(p);
    };
    applyBg(_terminal);
    applyBg(this);
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
