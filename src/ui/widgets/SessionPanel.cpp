/**
 * @file   SessionPanel.cpp
 * @brief  会话面板实现：历史会话树、重连与编辑。
 *
 * 通过 SessionStore 持久化会话历史，按连接类型分组展示。右键菜单支持
 * 重连、编辑、删除，面板顶部提供新建会话入口。
 */
#include "SessionPanel.h"

#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaTheme.h"
#include "ElaMenu.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTreeWidget.h"
#include "credential/CredentialStore.h"
#include "service/LanguageManager.h"
#include "session/SessionStore.h"
#include "ui/widgets/MessagePrompts.h"

#include <QDir>
#include <QDebug>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace {

// UUID 继续使用 Qt::UserRole，展示数据独立保存，不改变历史记录格式。
constexpr int DetailRole = Qt::UserRole + 1;
constexpr int KindRole = Qt::UserRole + 2;
constexpr int SessionTreeIndentation = 16;
constexpr int SessionIconSize = 24;
constexpr int GroupIconSize = 16;
constexpr int SessionFontPixelSize = 10;

/** @brief 单列会话委托：分组一行，会话以图标、名称和连接参数两行展示。 */
class SessionItemDelegate final : public QStyledItemDelegate
{
public:
    explicit SessionItemDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        QFont font = option.font;
        if (index.parent().isValid())
            font.setPixelSize(SessionFontPixelSize);

        const int lineHeight = QFontMetrics(font).height();
        return {0, index.parent().isValid()
                    ? std::max(38, 2 * lineHeight + 5)
                    : std::max(28, lineHeight + 5)};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const auto mode = eTheme->getThemeMode();
        const QColor foreground = ElaThemeColor(mode, BasicText);
        // 混合前景与背景得到次要文本色，深浅主题下都保留可读性。
        const QColor background = ElaThemeColor(mode, WindowBase);
        const QColor secondary((foreground.red() * 7 + background.red() * 3) / 10,
                               (foreground.green() * 7 + background.green() * 3) / 10,
                               (foreground.blue() * 7 + background.blue() * 3) / 10);
        const bool leaf = index.parent().isValid();
        const bool group = index.model()->hasChildren(index);
        // 仅收回叶子节点的层级缩进及图标宽度差，使名称与分组标题对齐。
        // 分组自身的图标、文字和展开箭头保持原位。
        const int leftShift = leaf
            ? SessionTreeIndentation + SessionIconSize - GroupIconSize : 0;
        const QRect contentRect = option.rect.adjusted(-leftShift, 0, 0, 0);
        const QRect row = contentRect.adjusted(0, 1, -2, -1);
        painter->save();
        painter->setClipRect(contentRect);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        if (option.state & QStyle::State_Selected) {
            // 选中项使用主题强调色，略高于悬停的辨识度，深色下稍加强。
            QColor selectedBackground = ElaThemeColor(mode, PrimaryNormal);
            selectedBackground.setAlpha(mode == ElaThemeType::Dark ? 56 : 45);
            painter->setBrush(selectedBackground);
            painter->drawRoundedRect(row, 5, 5);
        } else if (option.state & QStyle::State_MouseOver) {
            painter->setBrush(ElaThemeColor(mode, BasicHoverAlpha));
            painter->drawRoundedRect(row, 5, 5);
        }
        if (option.state & QStyle::State_HasFocus) {
            painter->setPen(QPen(ElaThemeColor(mode, PrimaryNormal), 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(row.adjusted(1, 1, -1, -1), 5, 5);
        }

        QRect textRect = row.adjusted(6, 0, -6, 0);
        if (leaf || group) {
            const int iconSize = leaf ? SessionIconSize : GroupIconSize;
            const QRect iconRect(textRect.left(), row.center().y() - iconSize / 2,
                                 iconSize, iconSize);
            if (leaf) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(ElaThemeColor(mode, BasicHoverAlpha));
                painter->drawRoundedRect(iconRect, 4, 4);
            }
            QFont iconFont(QStringLiteral("ElaAwesome"));
            iconFont.setPixelSize(14);
            painter->setFont(iconFont);
            painter->setPen(secondary);
            const auto icon = group ? ElaIconType::Folder
                : static_cast<TransportKind>(index.data(KindRole).toInt())
                        == TransportKind::LocalShell
                    ? ElaIconType::Laptop : ElaIconType::Server;
            painter->drawText(iconRect, Qt::AlignCenter, QChar(icon));
            textRect.setLeft(iconRect.right() + 9);
        }

        QFont textFont = option.font;
        if (leaf)
            textFont.setPixelSize(SessionFontPixelSize);
        textFont.setBold(group);
        painter->setFont(textFont);
        painter->setPen(group ? secondary : foreground);
        const int lineHeight = QFontMetrics(textFont).height();
        const QString title = index.data(Qt::DisplayRole).toString();
        const QString detail = index.data(DetailRole).toString();
        if (leaf && !detail.isEmpty()) {
            textRect.setTop(row.center().y() - lineHeight);
            textRect.setHeight(lineHeight);
        }
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(textFont).elidedText(title, Qt::ElideRight,
                                            std::max(0, textRect.width())));
        if (leaf && !detail.isEmpty()) {
            textRect.translate(0, lineHeight);
            painter->setPen(secondary);
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(textFont).elidedText(detail, Qt::ElideRight,
                                                std::max(0, textRect.width())));
        }
        painter->restore();
    }
};

QString localSessionName(TerminalView::LocalShellType type,
                         const QString& wslDistribution = {})
{
#ifdef Q_OS_WIN
    switch (type) {
    case TerminalView::LocalShellType::PowerShell:
        return QStringLiteral("powershell");
    case TerminalView::LocalShellType::Wsl:
        return wslDistribution.trimmed().isEmpty()
            ? QStringLiteral("WSL")
            : QStringLiteral("WSL (%1)").arg(wslDistribution.trimmed());
    case TerminalView::LocalShellType::Cmd:
        return QStringLiteral("cmd");
    }
#else
    Q_UNUSED(type);
    Q_UNUSED(wslDistribution);
    return QStringLiteral("shell");
#endif
    return QStringLiteral("shell");
}

QString sessionName(const RuntimeConfig& runtime)
{
    const QVariantMap& values = runtime.transport;
    switch (runtime.transportKind) {
    case TransportKind::LocalShell: {
        const auto type = static_cast<TerminalView::LocalShellType>(
            values.value(QStringLiteral("shellType")).toInt());
        return localSessionName(
            type, values.value(QStringLiteral("wslDistribution")).toString());
    }
    case TransportKind::Ssh:
        return QStringLiteral("%1@%2:%3")
            .arg(values.value(QStringLiteral("username")).toString(),
                 values.value(QStringLiteral("host")).toString())
            .arg(values.value(QStringLiteral("port")).toUInt());
    case TransportKind::Serial:
        return QStringLiteral("%1 @ %2")
            .arg(values.value(QStringLiteral("portName")).toString())
            .arg(values.value(QStringLiteral("baudRate")).toInt());
    case TransportKind::Telnet:
        return QStringLiteral("%1:%2")
            .arg(values.value(QStringLiteral("host")).toString())
            .arg(values.value(QStringLiteral("port")).toUInt());
    case TransportKind::Custom:
        return runtime.title.trimmed().isEmpty()
            ? SessionPanel::tr("Custom") : runtime.title;
    }
    return {};
}

QString transportGroupName(TransportKind kind)
{
    // 分组名称集中生成，确保树重建和运行时语言切换使用同一套文案。
    switch (kind) {
    case TransportKind::LocalShell:
        return SessionPanel::tr("Local terminals");
    case TransportKind::Ssh:
        return SessionPanel::tr("SSH hosts");
    case TransportKind::Serial:
        return SessionPanel::tr("Serial ports");
    case TransportKind::Telnet:
        return SessionPanel::tr("Telnet hosts");
    case TransportKind::Custom:
        return SessionPanel::tr("Other sessions");
    }
    return {};
}

QString sessionDetail(const RuntimeConfig& runtime)
{
    // 次要行保留连接参数，主标题优先展示用户标签。
    const QVariantMap& values = runtime.transport;
    switch (runtime.transportKind) {
    case TransportKind::LocalShell:
        return localSessionName(
            static_cast<TerminalView::LocalShellType>(
                values.value(QStringLiteral("shellType")).toInt()),
            values.value(QStringLiteral("wslDistribution")).toString());
    case TransportKind::Ssh:
        return sessionName(runtime);
    case TransportKind::Serial:
        // 用户标签作为主标题时，详情仍需同时给出设备与速率，避免同波特率的
        // 多个串口条目无法区分；格式与无标签时的默认会话名保持一致。
        return sessionName(runtime);
    case TransportKind::Telnet:
        return sessionName(runtime);
    case TransportKind::Custom:
        return {};
    }
    return {};
}

RuntimeConfig localRuntime(TerminalView::LocalShellType type,
                           const QString& wslDistribution,
                           const QString& label,
                           const QString& workingDirectory)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.title = localSessionName(type, wslDistribution);
    runtime.transport = {
        {QStringLiteral("shellType"), static_cast<int>(type)},
        // WSL 实例名独立持久化，保证编辑和重新连接仍指向同一发行版。
        {QStringLiteral("wslDistribution"), wslDistribution.trimmed()},
        {QStringLiteral("workingDirectory"), workingDirectory.trimmed()},
        {QStringLiteral("label"), label.trimmed()}};
    return runtime;
}

RuntimeConfig serialRuntime(const SerialConfig& config)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Serial;
    runtime.transport = {
        {QStringLiteral("portName"), config.portName},
        {QStringLiteral("baudRate"), config.baudRate},
        {QStringLiteral("dataBits"), static_cast<int>(config.dataBits)},
        {QStringLiteral("parity"), static_cast<int>(config.parity)},
        {QStringLiteral("stopBits"), static_cast<int>(config.stopBits)},
        {QStringLiteral("flowControl"), static_cast<int>(config.flowControl)},
        {QStringLiteral("reconnectSeconds"), config.reconnectSeconds},
        {QStringLiteral("label"), config.label}};
    runtime.title = sessionName(runtime);
    return runtime;
}

RuntimeConfig sshRuntime(const SshConfig& config)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Ssh;
    runtime.transport = {
        {QStringLiteral("host"), config.host},
        {QStringLiteral("username"), config.username},
        {QStringLiteral("port"), config.port},
        {QStringLiteral("authMethod"), config.authMethod},
        {QStringLiteral("privateKeyPath"), config.privateKeyPath},
        {QStringLiteral("terminalType"), config.terminalType},
        {QStringLiteral("keepAliveSeconds"), config.keepAliveSeconds},
        {QStringLiteral("label"), config.label}};
    runtime.title = sessionName(runtime);
    return runtime;
}

RuntimeConfig telnetRuntime(const TelnetConfig& config)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::Telnet;
    runtime.transport = {
        {QStringLiteral("host"), config.host},
        {QStringLiteral("port"), config.port},
        {QStringLiteral("terminalType"), config.terminalType},
        {QStringLiteral("naws"), config.naws},
        {QStringLiteral("binaryMode"), config.binaryMode},
        {QStringLiteral("keepAliveSeconds"), config.keepAliveSeconds},
        {QStringLiteral("label"), config.label}};
    runtime.title = sessionName(runtime);
    return runtime;
}

QByteArray sshSecret(const SshConfig& config)
{
    return config.authMethod == QStringLiteral("password")
        ? config.password.toUtf8() : config.keyPassphrase.toUtf8();
}

} // namespace

SessionPanel::SessionPanel(QWidget* parent)
    : QWidget(parent), _credentials(createCredentialStore())
{
    QDir dataDirectory(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dataDirectory.mkpath(QStringLiteral("."));
    _store = std::make_unique<SessionStore>(dataDirectory.filePath(QStringLiteral("session-history.json")));
    _entries = _store->load();
    bool historyNamesChanged = false;
    for (SessionRestoreMetadata& entry : _entries) {
        const QString name = sessionName(entry.runtimeSnapshot);
        if (entry.runtimeSnapshot.title != name) {
            entry.runtimeSnapshot.title = name;
            historyNamesChanged = true;
        }
    }
    if (historyNamesChanged)
        saveHistory();

    _rootLayout = new QVBoxLayout(this);
    _rootLayout->setContentsMargins(8, 12, 8, 12);
    _rootLayout->setSpacing(8);

    // 使用固定高度的操作容器，避免折叠后仅剩顶部布局时被纵向拉伸，
    // 从而保证展开图标始终停留在面板顶部。
    auto* headerWidget = new QWidget(this);
    headerWidget->setFixedHeight(34);
    auto* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    // 新建会话是面板的主要操作，直接占用原标题位置，减少一行重复的纵向空间。
    _newSessionButton = new ElaPushButton(headerWidget);
    _newSessionButton->setMinimumHeight(34);
    _newSessionButton->setBorderRadius(6);
    // 参考图的紫色主操作，仅应用于新建按钮，不覆盖全局主题。
    _newSessionButton->setLightDefaultColor(QColor("#8050B8"));
    _newSessionButton->setLightHoverColor(QColor("#7243AA"));
    _newSessionButton->setLightPressColor(QColor("#64369A"));
    _newSessionButton->setDarkDefaultColor(QColor("#9563CD"));
    _newSessionButton->setDarkHoverColor(QColor("#A273D6"));
    _newSessionButton->setDarkPressColor(QColor("#8050B8"));
    _newSessionButton->setLightTextColor(Qt::white);
    _newSessionButton->setDarkTextColor(Qt::white);
    QFont buttonFont = _newSessionButton->font();
    buttonFont.setBold(true);
    _newSessionButton->setFont(buttonFont);
    headerLayout->addWidget(_newSessionButton, 1);

    _collapseButton = new ElaIconButton(
        ElaIconType::AngleLeft, 12, 28, 28, headerWidget);
    headerLayout->addWidget(_collapseButton);
    _rootLayout->addWidget(headerWidget, 0, Qt::AlignTop);

    _searchEdit = new ElaLineEdit(this);
    _searchEdit->setFixedHeight(32);
    _searchEdit->setTextMargins(20, 0, 0, 0);
    auto* searchIcon = new ElaText(_searchEdit);
    searchIcon->setElaIcon(ElaIconType::MagnifyingGlass);
    searchIcon->setTextPixelSize(12);
    searchIcon->setGeometry(10, 0, 16, 32);
    searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    _searchEdit->setIsClearButtonEnable(true);
    _rootLayout->addWidget(_searchEdit);

    _tree = new ElaTreeWidget(this);
    // Qt 扩展选择：Ctrl 切换单项，Shift 选择锚点与点击项之间的整个范围。
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _tree->installEventFilter(this);

    QFont itemFont = _tree->font();
    itemFont.setPixelSize(13);
    _tree->setFont(itemFont);

    _tree->setColumnCount(1);
    _tree->setHeaderHidden(true);
    _tree->setAnimated(false);
    // 委托直接绘制单列内容，避开默认树样式额外的文本左边距。
    _tree->setItemDelegate(new SessionItemDelegate(_tree));
    _tree->setIsFrameVisible(false);
    _tree->setIndentation(SessionTreeIndentation);
    _tree->setRootIsDecorated(true);
    _tree->setUniformRowHeights(false);
    _tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _rootLayout->addWidget(_tree, 1);

    connect(_searchEdit, &QLineEdit::textChanged, this,
            [this]() { rebuildTree(); });
    connect(eTheme, &ElaTheme::themeModeChanged, _tree,
            [this]() { _tree->viewport()->update(); });

    connect(_collapseButton, &QPushButton::clicked, this,
            [this]() { setCollapsed(!_collapsed); });
    connect(_newSessionButton, &QPushButton::clicked, this,
            &SessionPanel::newSessionRequested);
    connect(_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { reconnectItem(item); });
    connect(_tree, &QWidget::customContextMenuRequested, this,
            &SessionPanel::showItemContextMenu);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    // 按钮文本与折叠提示的初始语言由 retranslateUi() 统一应用，
    // 后续语言切换也走同一函数，避免构造期另写一份 tr() 文案。
    retranslateUi();
}

SessionPanel::~SessionPanel() = default;

QList<SessionId> SessionPanel::selectedSessionIds() const
{
    QList<SessionId> ids;
    // 树遍历顺序与分组及叶子显示顺序一致，不依赖 Ctrl 点击的先后顺序。
    for (QTreeWidgetItemIterator it(_tree); *it; ++it) {
        const auto* item = *it;
        const SessionId id(item->data(0, Qt::UserRole).toString());
        if (item->isSelected() && !item->isHidden() && !id.isNull())
            ids.append(id);
    }
    return ids;
}

bool SessionPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _tree && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
            || key->key() == Qt::Key_Delete) {
            if (key->isAutoRepeat())
                return true;
            // 信号接收方可能立即更新历史并重建树，先复制 ID 再执行操作。
            const QList<SessionId> ids = selectedSessionIds();
            if (key->key() == Qt::Key_Delete)
                deleteSessions(ids);
            else {
                for (const SessionId& id : ids)
                    reconnectSession(id);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SessionPanel::setCollapsed(bool collapsed)
{
    if (_collapsed == collapsed)
        return;

    // 折叠前记录用户最后调整的宽度，展开时恢复而不是退回固定默认值。
    if (collapsed && width() >= MinimumExpandedWidth)
        _expandedWidth = width();

    _collapsed = collapsed;
    updateCollapsedUi();
    emit collapsedChanged(_collapsed);
    emit panelWidthChangeRequested(
        _collapsed ? CollapsedWidth : _expandedWidth);
}

void SessionPanel::setExpandedWidth(int width)
{
    _expandedWidth = std::max(MinimumExpandedWidth, width);
    if (!_collapsed)
        emit panelWidthChangeRequested(_expandedWidth);
}

void SessionPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!_collapsed && event->size().width() >= MinimumExpandedWidth)
        _expandedWidth = event->size().width();
}

void SessionPanel::updateCollapsedUi()
{
    // 折叠状态只保留一枚展开按钮，形成持续可见的窄侧栏；无需依赖顶栏菜单。
    _newSessionButton->setVisible(!_collapsed);
    _tree->setVisible(!_collapsed);
    _searchEdit->setVisible(!_collapsed);
    _collapseButton->setAwesome(
        _collapsed ? ElaIconType::AngleRight : ElaIconType::AngleLeft);
    _collapseButton->setAccessibleName(
        _collapsed ? tr("Expand quick connections")
                   : tr("Collapse quick connections"));
    _collapseButton->setToolTip(
        _collapsed ? tr("Expand quick connections")
                   : tr("Collapse quick connections"));

    if (_collapsed) {
        // 28px 按钮配合左右各 6px 边距，将折叠侧栏收窄到 40px。
        _rootLayout->setContentsMargins(6, 6, 6, 6);
        setMinimumWidth(CollapsedWidth);
        setMaximumWidth(CollapsedWidth);
    } else {
        _rootLayout->setContentsMargins(8, 12, 8, 12);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setMinimumWidth(MinimumExpandedWidth);
    }
    updateGeometry();
}

void SessionPanel::recordLocal(TerminalView::LocalShellType type,
                               const QString& wslDistribution,
                               const QString& label,
                               const QString& workingDirectory)
{
    upsert(localRuntime(type, wslDistribution, label, workingDirectory));
}

void SessionPanel::recordSerial(const SerialConfig& config)
{
    upsert(serialRuntime(config));
}

void SessionPanel::recordSsh(const SshConfig& config)
{
    upsert(sshRuntime(config), sshSecret(config));
}

void SessionPanel::recordTelnet(const TelnetConfig& config)
{
    upsert(telnetRuntime(config));
}

void SessionPanel::updateLocal(const SessionId& id,
                               TerminalView::LocalShellType type,
                               const QString& wslDistribution,
                               const QString& label,
                               const QString& workingDirectory)
{
    replace(id, localRuntime(type, wslDistribution, label, workingDirectory));
}

void SessionPanel::updateSerial(const SessionId& id,
                                const SerialConfig& config)
{
    replace(id, serialRuntime(config));
}

void SessionPanel::updateSsh(const SessionId& id, const SshConfig& config)
{
    replace(id, sshRuntime(config), sshSecret(config));
}

void SessionPanel::updateTelnet(const SessionId& id, const TelnetConfig& config)
{
    replace(id, telnetRuntime(config));
}

void SessionPanel::upsert(RuntimeConfig runtime, const QByteArray& secret)
{
    const QString key = runtimeKey(runtime);
    auto it = std::find_if(_entries.begin(), _entries.end(),
                           [this, &key](const SessionRestoreMetadata& entry) {
        return runtimeKey(entry.runtimeSnapshot) == key;
    });

    if (it == _entries.end()) {
        SessionRestoreMetadata entry;
        entry.sessionId = QUuid::createUuid();
        entry.reconnectOnRestore = false;
        _entries.append(std::move(entry));
        it = std::prev(_entries.end());
    }

    if (runtime.transportKind == TransportKind::Ssh && !secret.isEmpty()) {
        if (it->runtimeSnapshot.credentialRef.isEmpty()) {
            it->runtimeSnapshot.credentialRef =
                QStringLiteral("history-%1").arg(
                    it->sessionId.toString(QUuid::WithoutBraces));
        }
        runtime.credentialRef = it->runtimeSnapshot.credentialRef;
        if (!_credentials->put(runtime.credentialRef, secret))
            runtime.credentialRef.clear();
    } else if (!it->runtimeSnapshot.credentialRef.isEmpty()) {
        _credentials->remove(it->runtimeSnapshot.credentialRef);
    }

    it->runtimeSnapshot = std::move(runtime);
    saveHistory();
    rebuildTree();
}

void SessionPanel::replace(const SessionId& id, RuntimeConfig runtime,
                           const QByteArray& secret)
{
    const auto it = std::find_if(
        _entries.begin(), _entries.end(), [&id](const auto& entry) {
            return entry.sessionId == id;
        });
    if (it == _entries.end())
        return;

    const QString oldCredentialRef = it->runtimeSnapshot.credentialRef;
    if (runtime.transportKind == TransportKind::Ssh && !secret.isEmpty()) {
        runtime.credentialRef = oldCredentialRef.isEmpty()
            ? QStringLiteral("history-%1").arg(
                  id.toString(QUuid::WithoutBraces))
            : oldCredentialRef;
        if (!_credentials->put(runtime.credentialRef, secret))
            runtime.credentialRef.clear();
    } else if (!oldCredentialRef.isEmpty()) {
        _credentials->remove(oldCredentialRef);
    }

    it->runtimeSnapshot = std::move(runtime);
    saveHistory();
    rebuildTree();
}

QString SessionPanel::runtimeKey(const RuntimeConfig& runtime) const
{
    const auto& values = runtime.transport;
    switch (runtime.transportKind) {
    case TransportKind::LocalShell:
        return QStringLiteral("local:%1:%2")
            .arg(values.value(QStringLiteral("shellType")).toInt())
            .arg(values.value(QStringLiteral("wslDistribution")).toString());
    case TransportKind::Ssh:
        return QStringLiteral("ssh:%1@%2:%3:%4")
            .arg(values.value(QStringLiteral("username")).toString(),
                 values.value(QStringLiteral("host")).toString())
            .arg(values.value(QStringLiteral("port")).toUInt())
            .arg(values.value(QStringLiteral("authMethod")).toString());
    case TransportKind::Serial:
        return QStringLiteral("serial:%1:%2")
            .arg(values.value(QStringLiteral("portName")).toString())
            .arg(values.value(QStringLiteral("baudRate")).toInt());
    case TransportKind::Telnet:
        return QStringLiteral("telnet:%1:%2")
            .arg(values.value(QStringLiteral("host")).toString())
            .arg(values.value(QStringLiteral("port")).toUInt());
    case TransportKind::Custom:
        return QStringLiteral("custom:%1").arg(runtime.profileId);
    }
    return {};
}

void SessionPanel::saveHistory()
{
    QString error;
    if (!_store->save(_entries, &error))
        qWarning() << "Failed to save session history:" << error;
}

void SessionPanel::rebuildTree()
{
    _tree->clear();
    const QString query = _searchEdit->text().trimmed();

    // 固定分组顺序，避免会话保存顺序改变时侧栏类别来回跳动。
    const std::array kinds{
        TransportKind::LocalShell, TransportKind::Ssh,
        TransportKind::Serial, TransportKind::Telnet,
        TransportKind::Custom};
    for (const TransportKind kind : kinds) {
        QList<const SessionRestoreMetadata*> groupEntries;
        for (const SessionRestoreMetadata& entry : std::as_const(_entries)) {
            const auto& runtime = entry.runtimeSnapshot;
            const QString searchable = runtime.transport
                .value(QStringLiteral("label")).toString() + QLatin1Char(' ')
                + sessionName(runtime) + QLatin1Char(' ') + sessionDetail(runtime);
            if (runtime.transportKind == kind
                && (query.isEmpty() || searchable.contains(query, Qt::CaseInsensitive)))
                groupEntries.append(&entry);
        }
        if (groupEntries.isEmpty())
            continue;

        // 分组节点只负责展开/折叠，不代表具体会话，也不能触发右键操作。
        auto* group = new QTreeWidgetItem(_tree, {transportGroupName(kind)});
        group->setFlags(group->flags() & ~Qt::ItemIsSelectable);
        QFont groupFont = group->font(0);
        groupFont.setBold(true);
        group->setFont(0, groupFont);
        group->setExpanded(true);

        for (const SessionRestoreMetadata* entry : groupEntries) {
            const RuntimeConfig& runtime = entry->runtimeSnapshot;
            QString displayName = runtime.transport
                .value(QStringLiteral("label")).toString().trimmed();
            if (displayName.isEmpty()) {
                const auto& values = runtime.transport;
                if (kind == TransportKind::Serial)
                    displayName = values.value(QStringLiteral("portName")).toString();
                else if (kind == TransportKind::Ssh || kind == TransportKind::Telnet)
                    displayName = values.value(QStringLiteral("host")).toString();
                else {
                    displayName = sessionName(runtime);
                    if (displayName == QStringLiteral("powershell"))
                        displayName = QStringLiteral("PowerShell");
                    else if (displayName == QStringLiteral("cmd"))
                        displayName = QStringLiteral("CMD");
                }
            }

            auto* item = new QTreeWidgetItem(
                group, {displayName});
            item->setData(0, DetailRole, sessionDetail(runtime));
            item->setData(0, KindRole, static_cast<int>(kind));
            item->setData(0, Qt::AccessibleTextRole,
                          displayName + QStringLiteral(", ") + sessionDetail(runtime));
            item->setData(0, Qt::UserRole,
                          entry->sessionId.toString(QUuid::WithoutBraces));
            item->setToolTip(0, displayName + QLatin1Char('\n')
                + sessionDetail(runtime) + QLatin1Char('\n')
                + tr("Double-click to reconnect"));
        }
    }

    if (_tree->topLevelItemCount() == 0) {
        auto* emptyItem = new QTreeWidgetItem(
            _tree, {query.isEmpty() ? tr("No saved sessions yet")
                                    : tr("No matching sessions")});
        emptyItem->setFlags(emptyItem->flags() & ~Qt::ItemIsSelectable);
    }
}

void SessionPanel::showItemContextMenu(const QPoint& position)
{
    QTreeWidgetItem* const item = _tree->itemAt(position);
    // 只有携带会话 UUID 的叶子节点允许编辑或删除；分组和空状态节点跳过。
    if (!item || item->data(0, Qt::UserRole).toString().isEmpty())
        return;

    _tree->setCurrentItem(item);
    // 捕获稳定 SessionId 而非 item 指针：菜单是非模态的，关闭前树可能
    // 被 rebuildTree() 重建，item 指针随 clear() 失效。
    const SessionId id(item->data(0, Qt::UserRole).toString());
    auto* menu = new ElaMenu(_tree);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setMenuItemHeight(27);
    connect(menu->addElaIconAction(ElaIconType::PenToSquare, tr("Edit")),
            &QAction::triggered, this, [this, id]() { editItem(id); });
    connect(menu->addElaIconAction(ElaIconType::TrashCan, tr("Delete")),
            &QAction::triggered, this, [this, id]() { deleteItem(id); });
    menu->popup(_tree->viewport()->mapToGlobal(position));
}

void SessionPanel::editItem(const SessionId& id)
{
    const auto it = std::find_if(
        _entries.cbegin(), _entries.cend(), [&id](const auto& entry) {
            return entry.sessionId == id;
        });
    if (it == _entries.cend())
        return;

    QByteArray secret;
    if (!it->runtimeSnapshot.credentialRef.isEmpty()) {
        if (const auto stored =
                _credentials->get(it->runtimeSnapshot.credentialRef)) {
            secret = *stored;
        }
    }
    emit editSessionRequested(id, it->runtimeSnapshot, secret);
}

void SessionPanel::deleteItem(const SessionId& id)
{
    deleteSessions({id});
}

void SessionPanel::deleteSessions(const QList<SessionId>& ids)
{
    QStringList titles;
    for (const SessionId& id : ids) {
        const auto it = std::find_if(_entries.cbegin(), _entries.cend(),
            [&id](const auto& entry) { return entry.sessionId == id; });
        if (it != _entries.cend())
            titles.append(sessionName(it->runtimeSnapshot));
    }
    if (titles.isEmpty())
        return;

    const QString message = titles.size() == 1
        ? tr("Delete the saved session '%1'?").arg(titles.first())
        : tr("Delete these %1 saved sessions?\n%2")
            .arg(titles.size()).arg(titles.join(QLatin1Char('\n')));
    if (!NovaTerm::Ui::confirm(
            this, tr("Delete session"), message)) {
        return;
    }

    // 模态确认期间可能收到历史更新，确认后重新按 ID 查找，不保留旧迭代器。
    for (const SessionId& id : ids) {
        const auto it = std::find_if(_entries.begin(), _entries.end(),
            [&id](const auto& entry) { return entry.sessionId == id; });
        if (it == _entries.end())
            continue;
        if (!it->runtimeSnapshot.credentialRef.isEmpty())
            _credentials->remove(it->runtimeSnapshot.credentialRef);
        _entries.erase(it);
    }
    saveHistory();
    rebuildTree();
}

void SessionPanel::reconnectItem(QTreeWidgetItem* item)
{
    if (item)
        reconnectSession(SessionId(item->data(0, Qt::UserRole).toString()));
}

void SessionPanel::reconnectSession(const SessionId& id)
{
    if (id.isNull())
        return;

    const auto it = std::find_if(
        _entries.cbegin(), _entries.cend(), [&id](const auto& entry) {
            return entry.sessionId == id;
        });
    if (it == _entries.cend())
        return;

    const RuntimeConfig runtime = it->runtimeSnapshot;
    const QVariantMap& values = runtime.transport;
    if (runtime.transportKind == TransportKind::LocalShell) {
        emit localReconnectRequested(
            static_cast<TerminalView::LocalShellType>(
                values.value(QStringLiteral("shellType")).toInt()),
            values.value(QStringLiteral("wslDistribution")).toString(),
            values.value(QStringLiteral("label")).toString(),
            values.value(QStringLiteral("workingDirectory")).toString());
        return;
    }
    if (runtime.transportKind == TransportKind::Serial) {
        SerialConfig config;
        config.portName = values.value(QStringLiteral("portName")).toString();
        config.baudRate = values.value(QStringLiteral("baudRate")).toInt();
        config.dataBits = static_cast<QSerialPort::DataBits>(
            values.value(QStringLiteral("dataBits")).toInt());
        config.parity = static_cast<QSerialPort::Parity>(
            values.value(QStringLiteral("parity")).toInt());
        config.stopBits = static_cast<QSerialPort::StopBits>(
            values.value(QStringLiteral("stopBits")).toInt());
        config.flowControl = static_cast<QSerialPort::FlowControl>(
            values.value(QStringLiteral("flowControl")).toInt());
        config.reconnectSeconds = values.value(QStringLiteral("reconnectSeconds"), 0).toInt();
        config.label = values.value(QStringLiteral("label")).toString();
        if (config.isValid())
            emit serialReconnectRequested(config);
        return;
    }
    if (runtime.transportKind == TransportKind::Ssh) {
        SshConfig config;
        config.host = values.value(QStringLiteral("host")).toString();
        config.username = values.value(QStringLiteral("username")).toString();
        config.port = static_cast<quint16>(
            values.value(QStringLiteral("port")).toUInt());
        config.authMethod = values.value(QStringLiteral("authMethod")).toString();
        config.privateKeyPath = values.value(
            QStringLiteral("privateKeyPath")).toString();
        config.terminalType = values.value(QStringLiteral("terminalType")).toString();
        config.keepAliveSeconds = values.value(
            QStringLiteral("keepAliveSeconds")).toInt();
        config.label = values.value(QStringLiteral("label")).toString();
        if (!runtime.credentialRef.isEmpty()) {
            const auto secret = _credentials->get(runtime.credentialRef);
            if (secret) {
                if (config.authMethod == QStringLiteral("password"))
                    config.password = QString::fromUtf8(*secret);
                else
                    config.keyPassphrase = QString::fromUtf8(*secret);
            }
        }
        if (!config.isValid()) {
            // 凭据缺失与其它配置非法是两回事：密码认证下真正的动作是重新输入
            // 一次密码，而不是"重建会话"（旧文案对这种情况一直误导用户）。
            const bool passwordMissing =
                config.authMethod == QStringLiteral("password")
                && config.password.isEmpty();
            emit reconnectUnavailable(
                passwordMissing
                    ? tr("The saved password for this session is unavailable. "
                         "Edit the session and enter the password again.")
                    : tr("The saved SSH credential is unavailable. Create the "
                         "session again to refresh it."));
            return;
        }
        emit sshReconnectRequested(config);
        return;
    }
    if (runtime.transportKind == TransportKind::Telnet) {
        TelnetConfig config;
        config.host = values.value(QStringLiteral("host")).toString();
        config.port = static_cast<quint16>(
            values.value(QStringLiteral("port"), 23).toUInt());
        config.terminalType = values.value(
            QStringLiteral("terminalType"),
            QStringLiteral("xterm-256color")).toString();
        config.naws = values.value(QStringLiteral("naws"), true).toBool();
        config.binaryMode = values.value(
            QStringLiteral("binaryMode"), false).toBool();
        config.keepAliveSeconds = values.value(
            QStringLiteral("keepAliveSeconds"), 0).toInt();
        config.label = values.value(QStringLiteral("label")).toString();
        if (config.isValid())
            emit telnetReconnectRequested(config);
    }
}

void SessionPanel::retranslateUi()
{
    if (_newSessionButton) {
        _newSessionButton->setText(tr("+  New session"));
        _newSessionButton->setAccessibleName(tr("New session"));
    }
    _searchEdit->setPlaceholderText(tr("Search by name or host..."));
    _searchEdit->setAccessibleName(tr("Search sessions"));
    updateCollapsedUi();
    rebuildTree();
}
