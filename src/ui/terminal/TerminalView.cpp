/**
 * @file   TerminalView.cpp
 * @brief  终端视图实现：键盘/输出数据通路、PTY 尺寸去抖与搜索栏。
 *
 * 键盘事件经 TerminalRenderer → TerminalCore → ITransport::write；
 * ITransport::readyRead 经 TerminalCore::writeInput 喂入 libvterm 解析后
 * 触发渲染。resize 事件去抖合并，避免输出风暴。
 */
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

namespace {

TransportKind transportKindOf(ITransport* transport)
{
    // TerminalView 可能复用同一个 TerminalSession 承载不同后端，因此在
    // attach 时记录真实类型，不能沿用构造时的默认 LocalShell。
    if (qobject_cast<LocalShellTransport*>(transport))
        return TransportKind::LocalShell;
    if (qobject_cast<SshTransport*>(transport))
        return TransportKind::Ssh;
    // 部分轻量渲染测试不会链接 SerialTransport 实现，使用 Qt 元对象的
    // 运行时继承查询可避免为类型识别引入额外链接依赖。
    if (transport && transport->inherits("SerialTransport"))
        return TransportKind::Serial;
    return TransportKind::Custom;
}

} // namespace

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

static QByteArray terminalTitleSequence(QString title)
{
    // 防止标题内容提前终止 OSC 序列，恢复标题时仅保留普通文本。
    title.remove(QChar(0x1b));
    title.remove(QChar(0x07));
    QByteArray sequence = QByteArrayLiteral("\x1b]2;");
    sequence.append(title.toUtf8());
    sequence.append('\x07');
    return sequence;
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

    // 这些视图级连接在传输层切换时保持不变。放在 attachTransport() 之外，
    // 避免反复 attach/detach 导致标题与活动通知信号成倍增加。
    connect(_core, &TerminalCore::titleChanged, this,
            [this](const QString& title) {
        if (title.startsWith(QStringLiteral("NOVATERM_CWD_"))) {
            // 内部路径探针借用 OSC 2 传递结果；解析后立即恢复用户原有标题。
            _core->writeInput(terminalTitleSequence(_lastTerminalTitle));
            if (!_workingDirectoryRequestPending
                || (!_workingDirectoryMarker.isEmpty()
                    && !title.startsWith(_workingDirectoryMarker))) {
                return;
            }

            _workingDirectoryRequestPending = false;
            const QString path = title.mid(_workingDirectoryMarker.size());
            _workingDirectoryMarker.clear();
            if (path.startsWith(QLatin1Char('/')))
                emit workingDirectoryReported(path);
            else
                emit workingDirectoryRequestFailed();
            return;
        }

        _lastTerminalTitle = title;
        emit titleChanged(title);
    });
    connect(_renderer, &TerminalRenderer::activityDetected,
            this, &TerminalView::activityDetected);
    connect(_session, &TerminalSession::connected, this,
            [this](ITransport* transport) {
        if (!_session || _session->transport() != transport)
            return;

        // 重连成功后恢复显示层对同一 transport 的跟踪；否则第二次断连
        // 会被误认为不属于当前视图，无法再次显示重连提示。
        _displayTransport = transport;
        if (_session->runtimeConfig().transportKind == TransportKind::LocalShell) {
            _localTransport = transport;
            _isLocalShell = true;
        }
    });
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
            const QString message = _session && _session->canReconnect()
                ? QStringLiteral("\r\n\x1b[31m[连接已经断开] 按 Enter 重新连接\x1b[0m\r\n")
                : QStringLiteral("\r\n\x1b[31m[已断开连接]\x1b[0m\r\n");
            _core->writeInput(message.toUtf8());
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
        // QObject 按插入顺序销毁子对象。renderer 与 session 均持有 Core 的非拥有
        // 指针，故仅在所有依赖者都已 parent 到 View 之后才 adopt Core —— 这样它们
        // 会先于 Core 销毁。优化构建中此前的顺序在 TerminalView 拆卸时造成确定性
        // UAF，并破坏随后 QLineEdit/QWidgetLineControl 的析构。
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
    // 保留原单参数入口，兼容已有调用；WSL 的明确实例由双参数重载传入。
    startLocalShell(type, {});
}

void TerminalView::startLocalShell(LocalShellType type,
                                   const QString& wslDistribution)
{
    LocalShellConfig config;
#ifdef Q_OS_WIN
    // WSL 必须携带下拉框中实际发现的发行版名称，避免多实例环境下
    // 启动 wsl.exe 的默认实例而连接到错误的 Linux 系统。
    switch (type) {
    case LocalShellType::PowerShell:
        config.profile = LocalShellProfiles::windowsPowerShell();
        break;
    case LocalShellType::Wsl:
        config.profile = wslDistribution.trimmed().isEmpty()
            ? LocalShellProfiles::wsl()
            : LocalShellProfiles::wslDistribution(wslDistribution.trimmed());
        break;
    case LocalShellType::Cmd:
        config.profile = LocalShellProfiles::commandPrompt(
            QCoreApplication::applicationDirPath());
        break;
    }
#else
    Q_UNUSED(type);
    Q_UNUSED(wslDistribution);
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

    _session->attach(transport, TerminalSession::Ownership::Adopt,
                     transportKindOf(transport));
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

void TerminalView::pasteText(const QString& text)
{
    if (!_core || text.isEmpty())
        return;

    _core->pasteText(text);
    _renderer->setFocus(Qt::ShortcutFocusReason);
}

void TerminalView::submitText(const QString& text)
{
    if (!_core || text.isEmpty())
        return;

    // 粘贴与回车进入同一终端核心命令队列，保证命令完整写入后再执行。
    _core->pasteText(text);
    QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return,
                         Qt::NoModifier, QStringLiteral("\r"));
    _core->processKeyPress(&enterEvent);
    _renderer->setFocus(Qt::ShortcutFocusReason);
}

void TerminalView::requestWorkingDirectory()
{
    if (!_core || _workingDirectoryRequestPending)
        return;

    _workingDirectoryRequestPending = true;
    const quint64 generation = ++_workingDirectoryRequestGeneration;
    _workingDirectoryMarker = QStringLiteral("NOVATERM_CWD_%1:")
        .arg(generation);

    // printf 通过终端现有 OSC 2 解析通道返回 $PWD，不解析易受提示符影响的屏幕文本。
    submitText(QStringLiteral("printf '\\033]2;%1%s\\007' \"$PWD\"")
                   .arg(_workingDirectoryMarker));
    QTimer::singleShot(WorkingDirectoryRequestTimeoutMs, this,
                       [this, generation]() {
        if (!_workingDirectoryRequestPending
            || generation != _workingDirectoryRequestGeneration) {
            return;
        }
        _workingDirectoryRequestPending = false;
        _workingDirectoryMarker.clear();
        emit workingDirectoryRequestFailed();
    });
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
        // terminalSizeChanged 携带计算出的目标尺寸。该事件可能在 TerminalCore
        // 应用其异步 resize 之前就已到达。
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
