#include "TerminalView.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"

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

    // 监听终端 resize 事件，转发给当前 transport（跨平台通用）
    _terminal->installEventFilter(this);
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
//  本地终端模式 — 通过 LocalShellTransport 统一路径
//
//  数据流：
//    键盘 → QTermWidget::sendData 信号 → LocalShellTransport::write()
//         → PTY master fd / ConPTY input pipe → shell stdin
//
//    shell stdout → PTY master fd / ConPTY output pipe
//         → LocalShellTransport::readyRead 信号
//         → QTermWidget::receiveData → VT 解析 → 渲染
//
//  这条路径与远程传输（SSH/Serial/Telnet）完全一致，
//  只是 ITransport 的具体实现不同。
// ═══════════════════════════════════════════════════════════════════

void TerminalView::startLocalShell(LocalShellType type)
{
    stopLocalShell();
    detachTransport();

    auto* transport = new LocalShellTransport(this);

    // ── 配置 shell ────────────────────────────────────────────
#ifdef Q_OS_WIN
    if (type == LocalShellType::PowerShell) {
        transport->setShellProgram(QStringLiteral("powershell.exe"));
    } else {
        const QString clinkBat = resolveClinkBat();
        if (!clinkBat.isEmpty()) {
            // cmd.exe /k "<clink.bat>" inject — 引号包裹路径处理空格
            transport->setShellProgram(QStringLiteral("cmd.exe"));
            transport->setShellArgs({
                QStringLiteral("/k"),
                QLatin1Char('"') + clinkBat + QLatin1Char('"'),
                QStringLiteral("inject")
            });
        } else {
            const QString comSpec = QString::fromLocal8Bit(qgetenv("ComSpec"));
            transport->setShellProgram(comSpec.isEmpty() ? QStringLiteral("cmd.exe") : comSpec);
            qWarning() << "TerminalView: 未找到 clink.bat，回退到 cmd.exe";
        }
    }
#else
    Q_UNUSED(type);
    // Unix：使用用户默认 shell
    QString shell = QString::fromLocal8Bit(qgetenv("SHELL"));
    if (shell.isEmpty())
        shell = QStringLiteral("/bin/bash");
    transport->setShellProgram(shell);
#endif

    // 强制完成布局后再查询终端尺寸。addTerminalTab 中加入 Tab 后立即
    // 调用本方法，此时 Qt 布局尚未处理，_terminal 的 resizeEvent 未触发。
    // layout()->activate() 同步触发整个 resize 链，确保后续
    // screenColumnsCount()/screenLinesCount() 返回实际可视尺寸。
    if (auto* lay = layout())
        lay->activate();

    // 将终端尺寸传给 transport，PTY 以正确尺寸创建
    transport->resizeTerminal(_terminal->screenColumnsCount(),
                              _terminal->screenLinesCount());

    // ── 临时禁用 scrollback 以消除启动时滚动条异常下拉 ──────────
    // shell 通过 PTY/ConPTY 启动时的清屏序列会将 scrollbar 推到底部。
    // 解法：启动前将历史缓冲区大小置零，待 shell 启动序列（约 1500ms）
    // 结束后恢复原值。期间输出仍正常渲染，只是不写入 scrollback。
    const int savedHistorySize = _terminal->historySize();
    _terminal->setHistorySize(0);

    // 通过统一的 ITransport 路径桥接
    attachTransport(transport);

    if (!transport->connectToHost()) {
        qWarning() << "TerminalView: LocalShellTransport 启动失败";
        _terminal->setHistorySize(savedHistorySize);
        detachTransport();
        emit shellFinished();
        return;
    }

    _isLocalShell = true;

    // shell 启动序列完成后恢复历史缓冲区
    QTimer::singleShot(1500, _terminal, [this, savedHistorySize]() {
        if (_terminal)
            _terminal->setHistorySize(savedHistorySize);
    });

    // transport 断开（shell 退出）→ 发射 shellFinished
    connect(transport, &ITransport::disconnected, this, [this]() {
        if (_isLocalShell) {
            _isLocalShell = false;
            emit shellFinished();
        }
    });
}

void TerminalView::stopLocalShell()
{
    if (!_isLocalShell)
        return;

    _isLocalShell = false;
    detachTransport();
}

// ═══════════════════════════════════════════════════════════════════
//  远程终端模式 — ITransport 数据桥接
//
//  数据流：键盘 → QTermWidget::sendData 信号
//                 → ITransport::write(bytes)
//                 → SshTransport / SerialTransport / ...
//
//         ITransport::readyRead 信号
//                 → QTermWidget::receiveData
//                 → VT 解析 → ScreenBuffer → 渲染
//
//  本地终端也走同一条路径，通过 LocalShellTransport 实现。
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
            this, [this](const QByteArray& data) {
        if (_terminal)
            _terminal->receiveData(data.constData(), data.size());
    });

    connect(_transport, &ITransport::disconnected,
            this, [this]() {
        if (_terminal) {
            const char* msg = "\r\n[已断开连接]\r\n";
            _terminal->receiveData(msg, static_cast<int>(strlen(msg)));
        }
    });

    // 转发终端键盘输入 → transport
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
    menu->setAttribute(Qt::WA_DeleteOnClose);
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

// ═══════════════════════════════════════════════════════════════════
//  eventFilter — 终端尺寸变更转发给 transport
// ═══════════════════════════════════════════════════════════════════

bool TerminalView::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == _terminal && event->type() == QEvent::Resize) {
        // 延迟到事件循环下一轮同步尺寸：QTermWidget 内部 TerminalDisplay
        // 在 resize 事件处理后才重新计算行列数，同步调用可能拿到旧值。
        QMetaObject::invokeMethod(this, [this]() {
            if (_transport && _transport->isConnected()) {
                const int cols = _terminal->screenColumnsCount();
                const int lines = _terminal->screenLinesCount();
                if (cols > 0 && lines > 0)
                    _transport->resizeTerminal(cols, lines);
            }
        }, Qt::QueuedConnection);
    }
    return QWidget::eventFilter(obj, event);
}
