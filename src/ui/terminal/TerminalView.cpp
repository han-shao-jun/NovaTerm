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
#include "ui/widgets/MessagePrompts.h"
#include "ui/widgets/SerialFileTransferDialog.h"
#include "session/transfer/SerialFileTransferController.h"
#include "core/terminal/TerminalCore.h"
#include "renderer/TerminalRenderer.h"
#include "renderer/TerminalColorScheme.h"
#include "service/ConfigManager.h"
#include "service/TerminalSchemeStore.h"
#include "service/LanguageManager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include "ElaLineEdit.h"
#include "ElaMenu.h"
#include "ElaScrollBar.h"
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTimer>
#include <QContextMenuEvent>
#include <QWheelEvent>

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

// 是否应答 OSC 52 读取查询（terminal.osc52ClipboardRead，默认 false）。
// 应答等于允许远端程序读走本机剪贴板，保守默认关闭。
static bool configuredOsc52ClipboardRead()
{
    return ConfigManager::instance().root()
        .value(QStringLiteral("terminal")).toObject()
        .value(QStringLiteral("osc52ClipboardRead")).toBool(false);
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
        // Core 默认不限历史行数，四种 Transport 共用字节预算。
    }
    _latestResizeColumns = _core->columns();
    _latestResizeRows = _core->rows();
    // OSC 52 读取查询默认关闭（剪贴板外泄防护），仅配置显式开启时应答。
    // 自建与外部注入的 core 都要接（scrollback 只在自建路径接是因为外部
    // core 由会话方配置历史；本开关任何 core 默认都应为关）。
    _core->setClipboardQueryAnswerEnabled(configuredOsc52ClipboardRead());
    _renderer = new TerminalRenderer(_core, this);
    _session = session ? session : new TerminalSession(_core, this);

    applyColorScheme();

    // 布局
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    _searchLine = new ElaLineEdit(this);
    _searchLine->setClearButtonEnabled(true);
    _searchLine->hide();
    layout->addWidget(_searchLine);

    // 终端与右侧滚动条并排。滚动条常驻布局而非按有无历史显示/隐藏：
    // 隐藏会改变渲染器宽度，进而改变列数并触发一次整段历史重排，表现为
    // 首次产生历史时内容跳动。无历史时量程为 0，Ela 按满长滑块绘制，语义
    // 上正是"视口覆盖全部内容"。
    auto* terminalRow = new QHBoxLayout;
    terminalRow->setContentsMargins(0, 0, 0, 0);
    terminalRow->setSpacing(0);
    terminalRow->addWidget(_renderer);
    _scrollBar = new ElaScrollBar(Qt::Vertical, this);
    _scrollBar->setScrollBarExtent(16);
    // 量程以显示行为单位，单步一行；页步在终端尺寸变化时同步为可见行数，
    // 使滑块长度反映"视口占全部内容的比例"。
    _scrollBar->setSingleStep(1);
    _scrollBar->setPageStep(_latestResizeRows);
    _scrollBar->setRange(0, 0);
    terminalRow->addWidget(_scrollBar);
    layout->addLayout(terminalRow);

    setFocusProxy(_renderer);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested,
            this, &TerminalView::setupContextMenu);

    // 终端自身选择深浅分类，窗口的 ElaTheme 变化不改变终端颜色。
    connect(&ConfigManager::instance(), &ConfigManager::configChanged, this,
            [this](const QString& path) {
        if (path != QStringLiteral("schemes")
            && path != QStringLiteral("terminal.appearance")
            && path != QStringLiteral("terminal.colorScheme"))
            return;
        if (_colorSchemeUpdatePending)
            return;
        _colorSchemeUpdatePending = true;
        QTimer::singleShot(0, this, [this] {
            _colorSchemeUpdatePending = false;
            applyColorScheme();
        });
    });

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
        request.query = text.toStdString();
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
        _serialReconnectPromptActive = false;
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
            // 状态提示写入终端流而非控件文本，仍走 tr() 使文案跟随界面语言。
            const QString message = _session && _session->canReconnect()
                ? tr("[Disconnected] Press Enter to reconnect.")
                : tr("[Disconnected].");
            _core->writeInput(
                QStringLiteral("\r\n\x1b[0;33m%1\x1b[0m\r\n")
                    .arg(message).toUtf8());
        }
    });
    connect(_session, &TerminalSession::errorOccurred, this,
            [this](ITransport*, const QString& error) {
        // 串口详细打开错误只在新会话首次连接阶段显示，重连使用等待提示。
        if (_session && _session->runtimeConfig().transportKind == TransportKind::Serial
            && (_session->statistics().connectedAt.isValid()
                || _session->statistics().reconnectCount > 0))
            return;
        if (_core && !error.isEmpty()) {
            _core->writeInput(
                QStringLiteral("\r\n\x1b[0;31m%1\x1b[0m\r\n")
                    .arg(tr("[Transport error] %1").arg(error)).toUtf8());
        }
    });
    connect(_session, &TerminalSession::automaticReconnectAttemptStarted, this, [this] {
        if (!_core)
            return;
        if (!_serialReconnectPromptActive) {
            _serialReconnectPromptActive = true;
            _core->writeInput(QStringLiteral("\r\n\x1b[0;33m%1\x1b[0m")
                .arg(tr("[Reconnecting] Waiting for the serial port")).toUtf8());
        }
        // 每次定时重试追加一个点，不重复输出整行错误或状态文本。
        _core->writeInput(QByteArrayLiteral("\x1b[0;33m.\x1b[0m"));
    });
    connect(_session, &TerminalSession::stateChanged, this, [this](SessionState state) {
        if (_serialReconnectPromptActive
            && (state == SessionState::Closing || state == SessionState::Closed)) {
            _serialReconnectPromptActive = false;
            if (_core)
                _core->writeInput(QByteArrayLiteral("\r\n"));
        }
    });
    connect(_session, &TerminalSession::automaticReconnectSucceeded, this, [this] {
        if (!_core)
            return;
        // ANSI 绿色由当前终端方案的调色板解析，深浅不跟随程序主题。
        _core->writeInput(QStringLiteral("\r\n\x1b[0;32m%1\x1b[0m\r\n")
            .arg(tr("[Reconnected] Automatic reconnection succeeded.")).toUtf8());
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
        // 页步即可见行数：决定滑块长度与点击空白处的翻页幅度。
        _scrollBar->setPageStep(rows);
        _resizeDebounce->start();
    });

    // ── 右侧滚动条 ⇄ 渲染器回看偏移 ──────────────────────────
    // 渲染器是唯一的滚动状态来源：它在滚轮、键盘回底、历史追加/淘汰与重排
    // 完成后发布 scrollStateChanged，此处只做坐标换算并写入滚动条。
    connect(_renderer, &TerminalRenderer::scrollStateChanged,
            this, &TerminalView::syncScrollBar);
    connect(_scrollBar, &QAbstractSlider::valueChanged, this, [this](int value) {
        if (_scrollBarSyncing)
            return;
        // 反向换算：滚动条顶部（0）是最旧历史，底部（maximum）是实时底部。
        _renderer->scrollToLine(_scrollBar->maximum() - value);
    });
    syncScrollBar(_renderer->maximumScrollOffset(), _renderer->scrollOffset());

    if (_ownsSession) {
        // QObject 按插入顺序销毁子对象。renderer 与 session 均持有 Core 的非拥有
        // 指针，故仅在所有依赖者都已 parent 到 View 之后才 adopt Core —— 这样它们
        // 会先于 Core 销毁。优化构建中此前的顺序在 TerminalView 拆卸时造成确定性
        // UAF，并破坏随后 QLineEdit/QWidgetLineControl 的析构。
        _core->setParent(this);
        static_cast<void>(ownedCore.release());
    }

    // 运行时语言切换：搜索栏占位符等常驻文本需重新应用 tr() 结果。
    // 右键菜单每次弹出时重建（setupContextMenu），其文本在弹出瞬间已取当前语言。
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    retranslateUi();
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
    startLocalShell(type, wslDistribution, {});
}

void TerminalView::startLocalShell(LocalShellType type,
                                   const QString& wslDistribution,
                                   const QString& workingDirectory)
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
    config.workingDirectory = workingDirectory.trimmed();
    startLocalShell(config);
}

void TerminalView::startLocalShell(const LocalShellConfig& config)
{
    stopLocalShell();
    detachTransport();

    const QString executableName =
        QFileInfo(config.profile.executable).fileName();
    _renderer->setConservativeLiveScrollRendering(
        executableName.compare(QStringLiteral("wsl.exe"),
                               Qt::CaseInsensitive) != 0);

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

    // attachTransport 使用 renderer 发布的最新目标尺寸创建 PTY。
    // Core 的 resize 异步执行，此处不能用尚未更新的模型尺寸覆盖目标。

    // ── 临时禁用 scrollback 以消除启动时滚动条异常 ──────────
    // 恢复不限行数策略，不能重新引入旧配置的固定行数上限。
    const int savedHistorySize = TerminalCore::UnlimitedScrollbackLines;
    _core->setScrollbackLimit(0);

    // 通过统一的 ITransport 路径桥接
    if (!attachTransport(transport)) {
        // attach 失败：transport 无 parent、未被 session adopt，
        // 所有权仍在本地 —— 必须在此回收，否则泄漏且 _localTransport
        // 会指向野指针。
        delete transport;
        _core->setScrollbackLimit(savedHistorySize);
        emit shellFinished();
        return;
    }

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

bool TerminalView::attachTransport(ITransport* transport, bool lfImpliesCr)
{
    detachTransport();
    if (!transport)
        return false;
    if (_session->state() == SessionState::Closed
        && !_session->resetForReuse()) {
        qWarning() << "TerminalView: failed to prepare the next session";
        return false;
    }

    // 解析兼容模式在字节进入 InputPump 前排队；其他传输默认关闭。
    // 队列拒绝设置时不可继续启动，否则显示行为会与会话配置不符。
    if (!_core->setLfImpliesCr(lfImpliesCr)) {
        qWarning() << "TerminalView: failed to queue LF compatibility mode";
        return false;
    }

    // libvterm 无需 "teletype" 模式 — 它本身不内置 PTY，
    // 所有 I/O 都通过回调/API 驱动。

    _session->attach(transport, TerminalSession::Ownership::Adopt,
                     transportKindOf(transport));
    _displayTransport = transport;

    // SSH 在打开 channel 前需要正确的 PTY 尺寸；Serial/Local 对 resize
    // 是幂等或 no-op，统一传入无副作用。
    _session->resize(_latestResizeColumns, _latestResizeRows);

    // SSH 专属：主机密钥首次信任 / 变更必须经用户确认（P6 禁止静默接受）。
    //
    // 传输层是视图的子对象，而确认对话框的 exec() 会开启嵌套模态事件循环：
    // 该循环期间视图可能被任何带外拆卸销毁，随后的连接断开会连带析构
    // TerminalSession 与传输层，exec() 于是退回到一个仍在运行的 lambda，
    // 此时对 ssh 调 acceptHostKey()/rejectHostKey() 等于写已释放内存。
    // 因此这里只捕获 QPointer，弹窗返回后重新判活再触碰传输层。
    if (auto* ssh = qobject_cast<SshTransport*>(transport)) {
        const QPointer<SshTransport> sshGuard(ssh);
        connect(sshGuard, &SshTransport::hostKeyRequired, this,
                [this, sshGuard](const SshHostKeyInfo& info) {
            if (!sshGuard)
                return;
            // 按项目约定挂到窗口一级（Ela 对话框的 parent 不能为空，
            // 且浮层类控件必须落在窗口层），这样对话框比视图活得更久。
            // 此处取 window() 时视图必然健在——它尚未进入任何嵌套循环。
            auto* dialog = new SshHostKeyDialog(info, window());
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            const bool accepted = dialog->exec() == QDialog::Accepted;
            if (!sshGuard)
                return;
            if (accepted)
                sshGuard->acceptHostKey();
            else
                sshGuard->rejectHostKey();
        });
    }

    return true;
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
    if (!_core || text.isEmpty() || fileTransferActive())
        return;

    _core->pasteText(text);
    _renderer->setFocus(Qt::ShortcutFocusReason);
}

void TerminalView::pasteFromClipboard()
{
    pasteText(QApplication::clipboard()->text(QClipboard::Clipboard));
}

void TerminalView::submitText(const QString& text)
{
    if (!_core || text.isEmpty() || fileTransferActive())
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
    if (!_core || _workingDirectoryRequestPending || fileTransferActive())
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

void TerminalView::applyColorScheme()
{
    if (!_renderer)
        return;

    const bool isDark = ConfigManager::get<QString>(
        QStringLiteral("terminal.appearance"), QStringLiteral("dark")) != QStringLiteral("light");
    QString error;
    const TerminalColorScheme scheme = TerminalSchemeStore::resolve(
        ConfigManager::instance().root(), isDark, &error);
    if (!error.isEmpty())
        qWarning().noquote() << error;

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

bool TerminalView::fileTransferActive()
{
    // Session 门面按需创建控制器；检查到串口后记录观察指针，输入不经 UI
    // 保存或转发。断线和重绑期间旧控制器仍需保持门禁直至 Session 中止它。
    if (!_serialFileTransfer && _session && _session->canTransferFiles())
        _serialFileTransfer = _session->serialFileTransfer();
    return _serialFileTransfer && _serialFileTransfer->isActive();
}

void TerminalView::showFileTransfer()
{
    if (!_session || !_session->canTransferFiles())
        return;
    auto* controller = _session->serialFileTransfer();
    if (!controller)
        return;
    if (_transferDialog && _serialFileTransfer != controller) {
        // 重绑后旧窗口观察的控制器可能已经析构，不能复用它启动新通道。
        _transferDialog->deleteLater();
        _transferDialog.clear();
    }
    _serialFileTransfer = controller;
    if (!_transferDialog) {
        _transferDialog = new SerialFileTransferDialog(controller,
            NovaTerm::FileTransfer::Direction::Send, this);
    }
    _transferDialog->show();
    _transferDialog->raise();
    _transferDialog->activateWindow();
}

void TerminalView::setupContextMenu(const QPoint& pos)
{
    auto* menu = new ElaMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setMenuItemHeight(27);

    connect(menu->addElaIconAction(ElaIconType::Copy, tr("Copy")),
            &QAction::triggered, _renderer, &TerminalRenderer::copySelection);

    auto* paste = menu->addElaIconAction(ElaIconType::Paste, tr("Paste"));
    paste->setEnabled(!fileTransferActive());
    connect(paste, &QAction::triggered, this, &TerminalView::pasteFromClipboard);

    if (_session && _session->canTransferFiles()) {
        menu->addSeparator();
        connect(menu->addElaIconAction(ElaIconType::FileArrowUp, tr("Serial File Transfer…")),
                &QAction::triggered, this, &TerminalView::showFileTransfer);
    }

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlass, tr("Find...")),
            &QAction::triggered, this, &TerminalView::showSearch);

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassPlus, tr("Zoom In")),
            &QAction::triggered, _renderer, &TerminalRenderer::zoomIn);

    connect(menu->addElaIconAction(ElaIconType::MagnifyingGlassMinus, tr("Zoom Out")),
            &QAction::triggered, _renderer, &TerminalRenderer::zoomOut);

    menu->addSeparator();

    // 回看位置快速跳转；已在对应端时置灰。
    QAction* scrollTopAction =
        menu->addElaIconAction(ElaIconType::ArrowUpToLine, tr("Scroll to Top"));
    scrollTopAction->setEnabled(
        _renderer->scrollOffset() < _renderer->maximumScrollOffset());
    connect(scrollTopAction, &QAction::triggered,
            _renderer, &TerminalRenderer::scrollToTop);

    QAction* scrollBottomAction =
        menu->addElaIconAction(ElaIconType::ArrowDownToLine, tr("Scroll to Bottom"));
    scrollBottomAction->setEnabled(_renderer->scrollOffset() > 0);
    connect(scrollBottomAction, &QAction::triggered,
            _renderer, &TerminalRenderer::scrollToBottom);

    menu->addSeparator();

    connect(menu->addElaIconAction(ElaIconType::Broom, tr("Clear Scrollback")),
            &QAction::triggered, _core, &TerminalCore::clearScrollback);

    connect(menu->addElaIconAction(ElaIconType::BroomWide, tr("Clear All")),
            &QAction::triggered, this, [this] {
        if (!_core->clearAll()) {
            NovaTerm::Ui::warn(this, tr("Clear All Failed"),
                tr("The terminal is busy. Please try clearing it again shortly."));
            return;
        }
        _renderer->clearSelection();
        hideSearch();
        _renderer->setFocus(Qt::MouseFocusReason);
    });
    connect(menu->addElaIconAction(ElaIconType::SquareCheck, tr("Select All")),
            &QAction::triggered, _renderer, &TerminalRenderer::selectAll);

    menu->popup(mapToGlobal(pos));
}

// ═══════════════════════════════════════════════════════════════════
//  eventFilter — 终端粘贴、搜索快捷键与尺寸事件
// ═══════════════════════════════════════════════════════════════════

bool TerminalView::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == _renderer && fileTransferActive()) {
        if (event->type() == QEvent::ContextMenu) {
            auto* context = static_cast<QContextMenuEvent*>(event);
            setupContextMenu(mapFromGlobal(context->globalPos()));
            event->accept();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->matches(QKeySequence::Find)) {
                showSearch();
            } else if (key->key() == Qt::Key_Menu
                       || (key->key() == Qt::Key_F10
                           && key->modifiers() == Qt::ShiftModifier)) {
                setupContextMenu(_renderer->mapTo(this, _renderer->rect().center()));
            } else if (key->matches(QKeySequence::Copy)
                || (key->key() == Qt::Key_C
                    && key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))) {
                _renderer->copySelection();
            } else if (key->modifiers() == Qt::ShiftModifier
                       && (key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown)) {
                _renderer->scrollLines(key->key() == Qt::Key_PageUp
                    ? _core->rows() : -_core->rows());
            }
            event->accept();
            return true;
        }
        if (event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            const int delta = wheel->angleDelta().y() != 0
                ? wheel->angleDelta().y() : wheel->pixelDelta().y() * 3;
            _transferWheelAccum += delta;
            const int notches = _transferWheelAccum / 120;
            _transferWheelAccum -= notches * 120;
            if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
                for (int n = 0; n < std::abs(notches); ++n) {
                    if (notches > 0)
                        _renderer->zoomIn();
                    else
                        _renderer->zoomOut();
                }
            } else {
                _renderer->scrollLines(notches * 3);
            }
            event->accept();
            return true;
        }
        // IME、鼠标及焦点报告也会产生协议字节。准备阶段就消费它们，避免
        // 新命令持续进入 Parser 而阻塞旧输出屏障。本地复制和右键菜单仍可用。
        if (event->type() == QEvent::InputMethod
            || event->type() == QEvent::ShortcutOverride
            || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseMove
            || event->type() == QEvent::FocusIn
            || event->type() == QEvent::FocusOut) {
            event->accept();
            return true;
        }
    } else if (obj == _renderer) {
        _transferWheelAccum = 0;
    }
    if (obj == _renderer
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::MouseButtonRelease)) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::MiddleButton) {
            // 双击事件代表第二次按下；释放只消费，避免重复粘贴或向远端
            // 上报未配对的鼠标释放事件。
            if (event->type() != QEvent::MouseButtonRelease) {
                pasteFromClipboard();
                _renderer->setFocus(Qt::MouseFocusReason);
                emit activityDetected();
            }
            event->accept();
            return true;
        }
    }
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

void TerminalView::retranslateUi()
{
    // 搜索栏常驻显示，占位符需在语言切换后重新应用 tr() 结果；右键菜单每次
    // 弹出时重建，其文本无需在此刷新。
    if (_searchLine)
        _searchLine->setPlaceholderText(tr("Find in scrollback"));
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

void TerminalView::syncScrollBar(int maximumOffset, int offset)
{
    // 量程与取值必须在同一次同步里完成：历史追加会同时抬高 maximum 与
    // offset，若只 setRange 而不显式 setValue，停在实时底部的滑块会被留在
    // 旧的 maximum 上，看起来像自己往回滑进了历史。
    // 不用 QSignalBlocker 而用标志位：rangeChanged 仍需送达 ElaScrollBar
    // 自身的 onRangeChanged（维护其 _pTargetMaximum），只需让本类的
    // valueChanged 处理器认出这是同步写入、不要反向驱动渲染器。
    _scrollBarSyncing = true;
    _scrollBar->setRange(0, std::max(0, maximumOffset));
    _scrollBar->setValue(_scrollBar->maximum()
                         - std::clamp(offset, 0, _scrollBar->maximum()));
    _scrollBarSyncing = false;
}
