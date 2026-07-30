#include "TerminalView.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"
#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include "renderer/TerminalColorScheme.h"
#include "service/ConfigManager.h"

#include <QVBoxLayout>
#include "ElaMenu.h"
#include "ElaTheme.h"
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTimer>

#include <algorithm>

static QColor configuredColor(const QJsonObject& colors, const char* key,
                              const QColor& fallback)
{
    const QJsonValue value = colors.value(QLatin1String(key));
    if (!value.isString())
        return fallback;
    const QColor color = QColor::fromString(value.toString());
    return color.isValid() ? color : fallback;
}

static TerminalColorScheme configuredTerminalScheme(bool isDark)
{
    TerminalColorScheme scheme = isDark
        ? TerminalColorScheme::defaultDark()
        : TerminalColorScheme::defaultLight();

    const QJsonObject terminal =
        ConfigManager::instance().root().value(QStringLiteral("terminal")).toObject();
    const QString schemeName =
        terminal.value(QStringLiteral("colorScheme")).toString();
    if (schemeName.compare(QStringLiteral("system"), Qt::CaseInsensitive) == 0)
        return scheme;

    const QJsonObject colors = terminal.value(QStringLiteral("colors")).toObject();
    scheme.name = schemeName.isEmpty() ? QStringLiteral("Custom") : schemeName;
    scheme.foreground = configuredColor(colors, "foreground", scheme.foreground);
    scheme.background = configuredColor(colors, "background", scheme.background);
    scheme.cursorColor = configuredColor(colors, "cursor", scheme.cursorColor);
    scheme.selectionColor =
        configuredColor(colors, "selection", scheme.selectionColor);

    const QJsonArray palette = colors.value(QStringLiteral("palette")).toArray();
    if (palette.size() == 16) {
        for (int i = 0; i < 16; ++i) {
            if (!palette[i].isString())
                continue;
            const QColor color = QColor::fromString(palette[i].toString());
            if (color.isValid())
                scheme.palette[i] = color;
        }
    }
    return scheme;
}

TerminalView::TerminalView(QWidget* parent)
    : QWidget(parent)
{
    // ── 初始终端尺寸：用合理的默认值，resizeEvent 会马上更新 ──
    constexpr int kDefaultCols = 80;
    constexpr int kDefaultRows = 24;

    _core     = new TerminalCore(kDefaultCols, kDefaultRows, this);
    _renderer = new TerminalRenderer(_core, this);

    applyThemeColorScheme();

    // 布局
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(_renderer);

    setFocusProxy(_renderer);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &TerminalView::setupContextMenu);

    // 跟随 ElaTheme 明暗切换同步终端配色方案
    connect(eTheme, &ElaTheme::themeModeChanged,
            this, &TerminalView::applyThemeColorScheme);

    // 监听 renderer 的 resize 事件，转发给当前 transport
    _renderer->installEventFilter(this);

    // PTY 尺寸变更去抖：拖动窗口会产生密集的 resize 事件，每个都触发
    // 一次 SIGWINCH → shell 重绘，连续拖动即重绘风暴。合并为尺寸稳定后
    // 的单次通知。
    _resizeDebounce = new QTimer(this);
    _resizeDebounce->setSingleShot(true);
    _resizeDebounce->setInterval(80);
    connect(_resizeDebounce, &QTimer::timeout, this, [this]() {
        if (_transport && _transport->isConnected() && _core) {
            const int cols = _core->columns();
            const int rows = _core->rows();
            if (cols > 0 && rows > 0)
                _transport->resizeTerminal(cols, rows);
        }
    });
    connect(_renderer, &TerminalRenderer::terminalSizeChanged,
            _resizeDebounce, qOverload<>(&QTimer::start));
}

TerminalView::~TerminalView()
{
    stopLocalShell();
    detachTransport();
}

#ifdef Q_OS_WIN
// ── Clink 启动脚本定位（Windows）──────────────────────────────────
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
    QString shell = QString::fromLocal8Bit(qgetenv("SHELL"));
    if (shell.isEmpty())
        shell = QStringLiteral("/bin/bash");
    transport->setShellProgram(shell);
#endif

    // 强制完成布局后再查询终端尺寸
    if (auto* lay = layout())
        lay->activate();

    // 将终端尺寸传给 transport，PTY 以正确尺寸创建
    transport->resizeTerminal(_core->columns(), _core->rows());

    // ── 临时禁用 scrollback 以消除启动时滚动条异常 ──────────
    const int savedHistorySize = _core->scrollbackLineCount() > 0
        ? (std::max)(1000, _core->scrollbackLineCount()) : 1000;
    _core->setScrollbackLimit(0);

    // 通过统一的 ITransport 路径桥接
    attachTransport(transport);

    if (!transport->connectToHost()) {
        qWarning() << "TerminalView: LocalShellTransport 启动失败";
        _core->setScrollbackLimit(savedHistorySize);
        detachTransport();
        emit shellFinished();
        return;
    }

    _isLocalShell = true;

    // shell 启动序列完成后恢复历史缓冲区
    QTimer::singleShot(1500, this, [this, savedHistorySize]() {
        if (_core)
            _core->setScrollbackLimit(savedHistorySize);
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
// ═══════════════════════════════════════════════════════════════════

void TerminalView::attachTransport(ITransport* transport)
{
    detachTransport();
    stopLocalShell();
    _transport = transport;

    // libvterm 无需 "teletype" 模式 — 它本身不内置 PTY，
    // 所有 I/O 都通过回调/API 驱动。

    // Transport 输出 → libvterm 解析器
    connect(_transport, &ITransport::readyRead,
            this, [this](const QByteArray& data) {
        if (!_core || data.isEmpty())
            return;
        if (!_pendingTransportInput.isEmpty()) {
            _pendingTransportInput.append(data);
            if (_transport && !_transport->setReadPaused(true))
                emit _core->inputOverload(
                    QStringLiteral("transport cannot pause reads"));
            return;
        }
        for (qsizetype offset = 0; offset < data.size();
             offset += 64 * 1024) {
            const QByteArray chunk = data.mid(offset, 64 * 1024);
            if (_core->writeInput(chunk))
                continue;
            _pendingTransportInput.append(data.mid(offset));
            if (_transport && !_transport->setReadPaused(true))
                emit _core->inputOverload(
                    QStringLiteral("transport cannot pause reads"));
            break;
        }
    });

    connect(_core, &TerminalCore::inputBackpressureChanged,
            this, [this](bool paused) {
        if (!_transport)
            return;
        if (paused) {
            if (!_transport->setReadPaused(true))
                emit _core->inputOverload(
                    QStringLiteral("transport cannot pause reads"));
            return;
        }

        while (!_pendingTransportInput.isEmpty()) {
            const qsizetype length =
                qMin(qsizetype(64 * 1024),
                     _pendingTransportInput.size());
            const QByteArray chunk = _pendingTransportInput.left(length);
            if (!_core->writeInput(chunk)) {
                _transport->setReadPaused(true);
                return;
            }
            _pendingTransportInput.remove(0, length);
        }
        _transport->setReadPaused(false);
    });

    // 断开提示
    connect(_transport, &ITransport::disconnected,
            this, [this]() {
        if (_core) {
            const char* msg = "\r\n[已断开连接]\r\n";
            _core->writeInput(QByteArray(msg));
        }
    });

    // 转发终端键盘输入 → transport
    connect(_core, &TerminalCore::outputData,
            this, [this](const QByteArray& data) {
        if (_transport && _transport->isConnected())
            _transport->write(data);
    });

    // 转发标题变更
    connect(_core, &TerminalCore::titleChanged,
            this, [this](const QString& title) {
        emit titleChanged(title);
    });

    // 转发活动信号
    connect(_renderer, &TerminalRenderer::activityDetected,
            this, &TerminalView::activityDetected);
}

void TerminalView::detachTransport()
{
    if (_transport) {
        _transport->setReadPaused(false);
        disconnect(_transport, nullptr, this, nullptr);
        _transport = nullptr;
    }
    _pendingTransportInput.clear();
}

// ═══════════════════════════════════════════════════════════════════
//  主题适配
// ═══════════════════════════════════════════════════════════════════

void TerminalView::applyThemeColorScheme()
{
    if (!_renderer)
        return;

    bool isDark = (eTheme->getThemeMode() == ElaThemeType::Dark);
    const TerminalColorScheme scheme = configuredTerminalScheme(isDark);

    _renderer->setColorScheme(scheme);

    // 同步容器背景色
    QColor bg = scheme.background;
    setAutoFillBackground(true);
    QPalette p = QApplication::palette();
    p.setColor(backgroundRole(), bg);
    setPalette(p);
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
            &QAction::triggered, _renderer, &TerminalRenderer::copySelection);

    connect(menu->addElaIconAction(ElaIconType::Paste, tr("Paste")),
            &QAction::triggered, this, [this]() {
        if (_core) {
            const QString text = QApplication::clipboard()->text(
                QClipboard::Clipboard);
            _core->pasteText(text);
        }
    });

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassPlus, tr("Zoom In")),
            &QAction::triggered, _renderer, &TerminalRenderer::zoomIn);

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassMinus, tr("Zoom Out")),
            &QAction::triggered, _renderer, &TerminalRenderer::zoomOut);

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::Broom, tr("Clear Scrollback")),
            &QAction::triggered, _core, &TerminalCore::clearScrollback);

    menu->popup(mapToGlobal(pos));
}

// ═══════════════════════════════════════════════════════════════════
//  eventFilter — 终端尺寸变更转发给 transport
// ═══════════════════════════════════════════════════════════════════

bool TerminalView::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == _renderer && event->type() == QEvent::Resize) {
        // 去抖：重启定时器，只在尺寸稳定（80ms 内无新 resize 事件）后
        // 把当前终端尺寸同步给 PTY。拖动期间不会反复发送 SIGWINCH。
        // TerminalRenderer 已保证 _core 的尺寸不会跌到病态极小值，
        // 这里读取的 _core->columns()/rows() 始终是有效尺寸。
        if (_resizeDebounce)
            _resizeDebounce->start();
    }
    return QWidget::eventFilter(obj, event);
}
