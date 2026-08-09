#include "TerminalView.h"
#include "transport/ITransport.h"
#include "transport/LocalShellTransport.h"
#include "transport/SshTransport.h"
#include "session/TerminalSession.h"
#include "ui/widgets/SshHostKeyDialog.h"
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
#include <QDialog>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QTimer>

#include <algorithm>
#include <memory>

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
    : TerminalView(nullptr, parent)
{
}

TerminalView::TerminalView(TerminalSession* session, QWidget* parent)
    : QWidget(parent)
{
    // ── 初始终端尺寸：用合理的默认值，resizeEvent 会马上更新 ──
    constexpr int kDefaultCols = 80;
    constexpr int kDefaultRows = 24;

    _ownsSession = session == nullptr;
    auto ownedCore = std::unique_ptr<TerminalCore>{};
    if (session) {
        _core = session->core();
    } else {
        ownedCore = std::make_unique<TerminalCore>(kDefaultCols, kDefaultRows);
        _core = ownedCore.get();
    }
    _renderer = new TerminalRenderer(_core, this);
    _session = session ? session : new TerminalSession(_core, this);

    applyThemeColorScheme();

    // 布局
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    _searchLine = new QLineEdit(this);
    _searchLine->setPlaceholderText(tr("Find in scrollback"));
    _searchLine->setClearButtonEnabled(true);
    _searchLine->hide();
    layout->addWidget(_searchLine);
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
    _searchLine->installEventFilter(this);

    connect(_searchLine, &QLineEdit::textChanged, this,
            [this](const QString& text) {
        ++_searchGeneration;
        _renderer->clearSearchMatches();
        if (text.isEmpty()) {
            _core->cancelSearch(_searchGeneration);
            return;
        }
        NovaTerm::SearchRequest request;
        request.query = text;
        request.generation = _searchGeneration;
        request.resultBatchSize = 128;
        request.maximumResults = 100'000;
        _core->searchScrollback(std::move(request));
    });
    connect(_core, &TerminalCore::searchResultsReady, this,
            [this](const NovaTerm::SearchBatch& batch) {
        if (batch.generation != _searchGeneration)
            return;
        _renderer->appendSearchMatches(batch.matches, batch.generation);
    });

    // These view-level connections are invariant across transports. Keeping
    // them out of attachTransport() prevents repeated attach/detach cycles
    // from multiplying title and activity notifications.
    connect(_core, &TerminalCore::titleChanged,
            this, &TerminalView::titleChanged);
    connect(_renderer, &TerminalRenderer::activityDetected,
            this, &TerminalView::activityDetected);
    connect(_session, &TerminalSession::disconnected, this,
            [this](ITransport* transport) {
        const bool belongsToView = transport == _displayTransport;
        if (belongsToView)
            _displayTransport = nullptr;
        if (transport == _localTransport) {
            _localTransport = nullptr;
            _isLocalShell = false;
            emit shellFinished();
        }
        if (_core && belongsToView) {
            _core->writeInput(QByteArrayLiteral("\r\n[已断开连接]\r\n"));
        }
    });
    connect(_session, &TerminalSession::errorOccurred, this,
            [this](ITransport*, const QString& error) {
        if (_core && !error.isEmpty()) {
            _core->writeInput(
                QStringLiteral("\r\n[传输错误] %1\r\n").arg(error).toUtf8());
        }
    });

    // PTY 尺寸变更去抖：拖动窗口会产生密集的 resize 事件，每个都触发
    // 一次 SIGWINCH → shell 重绘，连续拖动即重绘风暴。合并为尺寸稳定后
    // 的单次通知。
    _resizeDebounce = new QTimer(this);
    _resizeDebounce->setSingleShot(true);
    _resizeDebounce->setInterval(80);
    connect(_resizeDebounce, &QTimer::timeout, this, [this]() {
        if (_session && _session->transport()
            && _latestResizeColumns > 0 && _latestResizeRows > 0) {
            _session->resize(_latestResizeColumns, _latestResizeRows);
        }
    });
    connect(_renderer, &TerminalRenderer::terminalSizeChanged,
            this, [this](int columns, int rows) {
        _latestResizeColumns = columns;
        _latestResizeRows = rows;
        _resizeDebounce->start();
    });

    if (_ownsSession) {
        // QObject deletes children in insertion order. Both renderer and
        // session retain non-owning pointers to Core, so adopt Core only after
        // all dependants have been parented to the View. They are therefore
        // destroyed first. In optimized builds the previous order caused a
        // deterministic UAF during TerminalView teardown and corrupted the
        // following QLineEdit/QWidgetLineControl destruction.
        _core->setParent(this);
        static_cast<void>(ownedCore.release());
    }
}

TerminalView::~TerminalView()
{
    if (_ownsSession) {
        stopLocalShell();
        detachTransport();
    } else if (_session) {
        QObject::disconnect(_session, nullptr, this, nullptr);
        _session = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  本地终端模式
// ═══════════════════════════════════════════════════════════════════

void TerminalView::startLocalShell(LocalShellType type)
{
    LocalShellConfig config;
#ifdef Q_OS_WIN
    config.profile = type == LocalShellType::PowerShell
        ? LocalShellProfiles::windowsPowerShell()
        : LocalShellProfiles::commandPrompt(QCoreApplication::applicationDirPath());
#else
    Q_UNUSED(type);
    config.profile = LocalShellProfiles::platformDefault();
#endif
    startLocalShell(config);
}

void TerminalView::startLocalShell(const LocalShellConfig& config)
{
    stopLocalShell();
    detachTransport();

    auto* transport = new LocalShellTransport;
    transport->setSessionConfig(config);
    connect(transport, &ITransport::disconnected, this, [this, transport] {
        if (_localTransport != transport)
            return;
        _localTransport = nullptr;
        _isLocalShell = false;
        emit shellFinished();
    });

    // 强制完成布局后再查询终端尺寸
    if (auto* lay = layout())
        lay->activate();

    // 将终端尺寸传给 transport，PTY 以正确尺寸创建
    _latestResizeColumns = _core->columns();
    _latestResizeRows = _core->rows();
    transport->resizeTerminal(_latestResizeColumns, _latestResizeRows);

    // ── 临时禁用 scrollback 以消除启动时滚动条异常 ──────────
    const int savedHistorySize = _core->scrollbackLineCount() > 0
        ? (std::max)(1000, _core->scrollbackLineCount()) : 1000;
    _core->setScrollbackLimit(0);

    // 通过统一的 ITransport 路径桥接
    attachTransport(transport);

    _localTransport = transport;
    if (!_session->start()) {
        qWarning() << "TerminalView: LocalShellTransport 启动失败";
        _core->setScrollbackLimit(savedHistorySize);
        _localTransport = nullptr;
        detachTransport();
        emit shellFinished();
        return;
    }

    _isLocalShell = true;

    // shell 启动序列完成后恢复历史缓冲区
    QTimer::singleShot(1500, this, [this, transport, savedHistorySize]() {
        if (_core && _localTransport == transport)
            _core->setScrollbackLimit(savedHistorySize);
    });
}

void TerminalView::stopLocalShell()
{
    if (!_localTransport)
        return;

    _isLocalShell = false;
    _session->detach();
}

// ═══════════════════════════════════════════════════════════════════
//  远程终端模式 — ITransport 数据桥接
// ═══════════════════════════════════════════════════════════════════

void TerminalView::attachTransport(ITransport* transport)
{
    detachTransport();
    if (!transport)
        return;
    if (_session->state() == SessionState::Closed
        && !_session->resetForReuse()) {
        qWarning() << "TerminalView: failed to prepare the next session";
        return;
    }

    // libvterm 无需 "teletype" 模式 — 它本身不内置 PTY，
    // 所有 I/O 都通过回调/API 驱动。

    _session->attach(transport, TerminalSession::Ownership::Adopt);
    _displayTransport = transport;

    // SSH 在打开 channel 前需要正确的 PTY 尺寸；Serial/Local 对 resize
    // 是幂等或 no-op，统一传入无副作用。
    _session->resize(_core->columns(), _core->rows());

    // SSH 专属：主机密钥首次信任 / 变更必须经用户确认（P6 禁止静默接受）。
    if (auto* ssh = qobject_cast<SshTransport*>(transport)) {
        connect(ssh, &SshTransport::hostKeyRequired, this,
                [this, ssh](const SshHostKeyInfo& info) {
            auto* dialog = new SshHostKeyDialog(info, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            if (dialog->exec() == QDialog::Accepted)
                ssh->acceptHostKey();
            else
                ssh->rejectHostKey();
        });
    }

}

void TerminalView::detachTransport()
{
    if (_session)
        _session->detach();
}

ITransport* TerminalView::transport() const
{
    return _session ? _session->transport() : nullptr;
}

TerminalSession* TerminalView::session() const
{
    return _session.data();
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

    connect(menu->addAction(tr("Find...")), &QAction::triggered,
            this, &TerminalView::showSearch);

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
    if (obj == _searchLine && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        hideSearch();
        return true;
    }
    if (obj == _renderer && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->matches(QKeySequence::Find)) {
            showSearch();
            return true;
        }
    }
    if (obj == _renderer && event->type() == QEvent::Resize) {
        // 去抖：重启定时器，只在尺寸稳定（80ms 内无新 resize 事件）后
        // 把当前终端尺寸同步给 PTY。拖动期间不会反复发送 SIGWINCH。
        // TerminalRenderer 已保证 _core 的尺寸不会跌到病态极小值，
        // 这里读取的 _core->columns()/rows() 始终是有效尺寸。
        // terminalSizeChanged carries the computed target dimensions. The
        // event itself may arrive before TerminalCore applies its async resize.
    }
    return QWidget::eventFilter(obj, event);
}

void TerminalView::showSearch()
{
    _searchLine->show();
    _searchLine->setFocus(Qt::ShortcutFocusReason);
    _searchLine->selectAll();
}

void TerminalView::hideSearch()
{
    ++_searchGeneration;
    _core->cancelSearch(_searchGeneration);
    _renderer->clearSearchMatches();
    _searchLine->hide();
    _renderer->setFocus(Qt::ShortcutFocusReason);
}
