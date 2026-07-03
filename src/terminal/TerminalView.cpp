#include "TerminalView.h"
#include "transport/ITransport.h"

#include "qtermwidget.h"
#include <QVBoxLayout>
#include "ElaMenu.h"
#include "ElaTheme.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTimer>

#ifdef _WIN32
#include "WinConPty.h"
#endif

TerminalView::TerminalView(QWidget* parent)
    : QWidget(parent)
{
    // ── 注册 QTermWidget 运行时数据目录 ──────────────────────────
    // QTermWidget 编译时硬编码的 COLORSCHEMES_DIR / KB_LAYOUT_DIR 是
    // 安装路径（如 /usr/share/qtermwidget6/），开发阶段从构建目录启动时
    // 这些路径不存在会触发警告。这里主动指向源码树中绑定的数据文件。
    {
        const QString dataRoot = QDir(QCoreApplication::applicationDirPath()
                + QStringLiteral("/../../third_party/qtermwidget/lib"))
                .absolutePath();

        const QString csDir = dataRoot + QStringLiteral("/color-schemes");
        if (QDir().exists(csDir))
            QTermWidget::addCustomColorSchemeDir(csDir);
    }

    // startnow=0：不在构造时自动启动 shell，由调用方决定本地/远程模式
    _terminal = new QTermWidget(0, this);

    // setCustomKeyBindingsDir 只能在 QTermWidget 构造后调用（是实例方法），
    // 而此时 "Unable to load translator 'default'" 警告已在 init() 内部触发。
    // 原因是 KeyboardTranslatorManager 标记为 KONSOLEPRIVATE_EXPORT（符号
    // 不出 DLL），外部无法在构造前预设路径。该警告无害 —— qtermwidget 会回退
    // 使用 DefaultTranslatorText.h 中的硬编码键盘映射。
    {
        const QString dataRoot = QDir(QCoreApplication::applicationDirPath()
                + QStringLiteral("/../../third_party/qtermwidget/lib"))
                .absolutePath();
        const QString kbDir = dataRoot + QStringLiteral("/kb-layouts");
        if (QDir().exists(kbDir))
            _terminal->setCustomKeyBindingsDir(kbDir);
    }

#ifdef _WIN32
    // ConPTY 尺寸须跟随终端显示区域变化，否则 shell 看到的行列数
    // 与实际渲染尺寸不一致，导致右侧滚动条异常下拉。
    _terminal->installEventFilter(this);
#endif

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

#ifdef Q_OS_WIN
// ── Clink 启动脚本定位（Windows）──────────────────────────────────
// cmd 类型时，在 NovaTerm.exe 同级目录查找 clink.bat（由 CMake 在生成
// 后事件中从 third_party/clink.1.9.27.83514e 复制过来）。找不到则返回
// 空字符串，调用方回退到裸 cmd.exe。
static QString resolveClinkBat()
{
    const QString bat = QCoreApplication::applicationDirPath()
                        + QStringLiteral("/clink.bat");
    QFileInfo fi(bat);
    return (fi.exists() && fi.isFile()) ? QDir::toNativeSeparators(fi.absoluteFilePath())
                                        : QString();
}
#endif // Q_OS_WIN

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

void TerminalView::startLocalShell(LocalShellType type)
{
    stopLocalShell();
    detachTransport();

#ifndef Q_OS_WIN
    // 非 Windows：shell 类型不适用（始终走 KPty 默认 shell），避免未使用参数告警。
    Q_UNUSED(type)
#endif

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

    // 按用户在会话对话框中选择的类型决定启动哪个 shell：
    //   • Cmd        → 通过 clink.bat inject 启动 Clink 增强版 cmd；
    //                   找不到 clink.bat 则回退裸 cmd.exe
    //   • PowerShell → powershell.exe（行为保持不变）
    QString shell;
    QStringList args;
    if (type == LocalShellType::PowerShell) {
        shell = QStringLiteral("powershell.exe");
    } else {
        const QString clinkBat = resolveClinkBat();
        if (!clinkBat.isEmpty()) {
            // cmd.exe /k "<clink.bat>" inject — batPath 被引号包裹以处理路径空格
            shell = QStringLiteral("cmd.exe");
            args << QStringLiteral("/k")
                 << (QLatin1Char('"') + clinkBat + QLatin1Char('"'))
                 << QStringLiteral("inject");
        } else {
            const QString comSpec = QString::fromLocal8Bit(qgetenv("ComSpec"));
            shell = comSpec.isEmpty() ? QStringLiteral("cmd.exe") : comSpec;
            qWarning() << "TerminalView: 未找到 clink.bat，回退到 cmd.exe";
        }
    }

    // 强制完成布局后再查询终端尺寸。addTerminalTab 中加入 Tab 后立即
    // 调用本方法，此时 Qt 布局尚未处理，_terminal 的 resizeEvent 未触发，
    // Emulation 内 Screen 对象仍为构造函数默认的 40×80 而非实际可视尺寸。
    // layout()->activate() 同步触发整个 resize 链：
    //   QTermWidget::resizeEvent → TerminalDisplay::updateImageSize()
    //   → changedContentSizeSignal → Session::updateTerminalSize()
    //   → Emulation::setImageSize() → Screen::resizeImage()
    // 此后 screenColumnsCount()/screenLinesCount() 返回的值与实际可视
    // 区域一致，ConPTY 以正确尺寸创建，shell 输出不会溢出导致滚动条下拉。
    if (auto* lay = layout())
        lay->activate();

    // ── 临时禁用 scrollback 以消除启动时的滚动条异常下拉 ──────────
    // cmd.exe 通过 ConPTY 启动时会将 FillConsoleOutputCharacter 等
    // Console API 调用翻译为 N 个 \r\n（N = 屏幕行数）来模拟清屏。
    // 第 N 个 \r\n 会超出可视区域底线，触发一行 scrollback 写入。
    // 这导致 setScroll(currentLine=1, lineCount=lines+1)，scrollbar
    // 的 value 被设为 maximum（即 1），滑块瞬间跌到底部。
    //
    // 解法：ConPTY 启动前将历史缓冲区大小置零，待 shell 启动序列
    // （约 1500ms）结束后恢复原值。期间 shell 输出仍正常渲染，只是
    // 不会写入 scrollback；用户在此期间也无法输入，无实际影响。
    const int savedHistorySize = _terminal->historySize();
    _terminal->setHistorySize(0);

    if (!_winPty->start(shell, args,
                         _terminal->screenColumnsCount(),
                         _terminal->screenLinesCount())) {
        qWarning() << "TerminalView: WinConPty 启动失败:" << shell << args;
        _terminal->setHistorySize(savedHistorySize);
        delete _winPty;
        _winPty = nullptr;
        emit shellFinished();
        return;
    }

    // shell 启动序列完成后恢复历史缓冲区。若发生竞态
    //（用户在恢复前通过 eventFilter resize 触发了更多输出），
    // 这些输出同样不会进入 scrollback，没有功能损失。
    QTimer::singleShot(1500, _terminal, [this, savedHistorySize]() {
        if (_terminal)
            _terminal->setHistorySize(savedHistorySize);
    });

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
        QDir(QCoreApplication::applicationDirPath()
             + QStringLiteral("/../../third_party/qtermwidget/lib/color-schemes"))
            .absolutePath() + QLatin1Char('/');

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

#ifdef _WIN32
bool TerminalView::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == _terminal && event->type() == QEvent::Resize) {
        // 延迟到事件循环下一轮再同步尺寸：QTermWidget 内部的
        // TerminalDisplay 会在 resize 事件处理后重新计算行列数，
        // 同步调用 screenColumnsCount()/screenLinesCount() 可能拿到旧值。
        QMetaObject::invokeMethod(this, [this]() {
            if (_winPty && _winPty->isRunning()) {
                const int cols = _terminal->screenColumnsCount();
                const int lines = _terminal->screenLinesCount();
                if (cols > 0 && lines > 0)
                    _winPty->resize(cols, lines);
            }
        }, Qt::QueuedConnection);
    }
    return QWidget::eventFilter(obj, event);
}
#endif
