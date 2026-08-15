/**
 * @file   SessionPanel.cpp
 * @brief  会话面板实现：历史会话树、重连与编辑。
 *
 * 通过 SessionStore 持久化会话历史，按连接类型分组展示。右键菜单支持
 * 重连、编辑、删除，面板顶部提供新建会话入口。
 */
#include "SessionPanel.h"

#include "ElaIconButton.h"
#include "ElaMenu.h"
#include "ElaPushButton.h"
#include "credential/CredentialStore.h"
#include "service/LanguageManager.h"
#include "session/SessionStore.h"

#include <QDir>
#include <QDebug>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

namespace {

QString localSessionName(TerminalView::LocalShellType type)
{
#ifdef Q_OS_WIN
    return type == TerminalView::LocalShellType::PowerShell
        ? QStringLiteral("powershell") : QStringLiteral("cmd");
#else
    Q_UNUSED(type);
    return QStringLiteral("shell");
#endif
}

QString sessionName(const RuntimeConfig& runtime)
{
    const QVariantMap& values = runtime.transport;
    switch (runtime.transportKind) {
    case TransportKind::LocalShell: {
        const auto type = static_cast<TerminalView::LocalShellType>(
            values.value(QStringLiteral("shellType")).toInt());
        return localSessionName(type);
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
    // 第一列展示用户可识别的名称，第二列只保留最关键的连接参数，
    // 避免窄侧栏内重复显示完整会话描述。
    const QVariantMap& values = runtime.transport;
    switch (runtime.transportKind) {
    case TransportKind::LocalShell:
        return localSessionName(static_cast<TerminalView::LocalShellType>(
            values.value(QStringLiteral("shellType")).toInt()));
    case TransportKind::Ssh:
        return QStringLiteral("%1:%2")
            .arg(values.value(QStringLiteral("host")).toString())
            .arg(values.value(QStringLiteral("port")).toUInt());
    case TransportKind::Serial:
        return QString::number(
            values.value(QStringLiteral("baudRate")).toInt());
    case TransportKind::Telnet:
        return values.value(QStringLiteral("host")).toString();
    case TransportKind::Custom:
        return {};
    }
    return {};
}

RuntimeConfig localRuntime(TerminalView::LocalShellType type,
                           const QString& label)
{
    RuntimeConfig runtime;
    runtime.transportKind = TransportKind::LocalShell;
    runtime.title = localSessionName(type);
    runtime.transport = {
        {QStringLiteral("shellType"), static_cast<int>(type)},
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

QByteArray sshSecret(const SshConfig& config)
{
    return config.authMethod == QStringLiteral("password")
        ? config.password.toUtf8() : config.keyPassphrase.toUtf8();
}

} // namespace

SessionPanel::SessionPanel(QWidget* parent)
    : QWidget(parent), _credentials(createCredentialStore())
{
    QDir dataDirectory(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    dataDirectory.mkpath(QStringLiteral("."));
    _store = std::make_unique<SessionStore>(
        dataDirectory.filePath(QStringLiteral("session-history.json")));
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
    _rootLayout->setContentsMargins(12, 12, 12, 12);
    _rootLayout->setSpacing(10);

    // 使用固定高度的标题容器，避免折叠后仅剩标题布局时被纵向拉伸，
    // 从而保证展开图标始终停留在面板顶部。
    auto* headerWidget = new QWidget(this);
    headerWidget->setFixedHeight(32);
    auto* headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);
    _titleLabel = new QLabel(tr("Quick connections"), this);
    QFont titleFont = _titleLabel->font();
    titleFont.setBold(true);
    _titleLabel->setFont(titleFont);
    headerLayout->addWidget(_titleLabel);
    headerLayout->addStretch();

    _collapseButton = new ElaIconButton(
        ElaIconType::AngleLeft, 12, 28, 28, headerWidget);
    headerLayout->addWidget(_collapseButton);
    _rootLayout->addWidget(headerWidget, 0, Qt::AlignTop);

    _newSessionButton = new ElaPushButton(tr("+  New session"), this);
    _newSessionButton->setAccessibleName(tr("New session"));
    _newSessionButton->setMinimumHeight(34);
    _rootLayout->addWidget(_newSessionButton);

    _tree = new QTreeWidget(this);
    _tree->setColumnCount(2);
    _tree->setHeaderHidden(true);
    _tree->setAnimated(false);
    _tree->setIndentation(16);
    _tree->setRootIsDecorated(true);
    _tree->setUniformRowHeights(true);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    _tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _tree->header()->setStretchLastSection(false);
    _tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _rootLayout->addWidget(_tree, 1);

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

    rebuildTree();
    updateCollapsedUi();
}

SessionPanel::~SessionPanel() = default;

void SessionPanel::setCollapsed(bool collapsed)
{
    if (_collapsed == collapsed)
        return;

    // 折叠前记录用户最后调整的宽度，展开时恢复而不是退回固定默认值。
    if (collapsed && width() >= 160)
        _expandedWidth = width();

    _collapsed = collapsed;
    updateCollapsedUi();
    emit collapsedChanged(_collapsed);
    emit panelWidthChangeRequested(
        _collapsed ? CollapsedWidth : _expandedWidth);
}

void SessionPanel::setExpandedWidth(int width)
{
    _expandedWidth = std::max(160, width);
    if (!_collapsed)
        emit panelWidthChangeRequested(_expandedWidth);
}

void SessionPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (!_collapsed && event->size().width() >= 160)
        _expandedWidth = event->size().width();
}

void SessionPanel::updateCollapsedUi()
{
    // 折叠状态只保留一枚展开按钮，形成持续可见的窄侧栏；无需依赖顶栏菜单。
    _titleLabel->setVisible(!_collapsed);
    _newSessionButton->setVisible(!_collapsed);
    _tree->setVisible(!_collapsed);
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
        _rootLayout->setContentsMargins(12, 12, 12, 12);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setMinimumWidth(160);
    }
    updateGeometry();
}

void SessionPanel::recordLocal(TerminalView::LocalShellType type,
                               const QString& label)
{
    upsert(localRuntime(type, label));
}

void SessionPanel::recordSerial(const SerialConfig& config)
{
    upsert(serialRuntime(config));
}

void SessionPanel::recordSsh(const SshConfig& config)
{
    upsert(sshRuntime(config), sshSecret(config));
}

void SessionPanel::updateLocal(const SessionId& id,
                               TerminalView::LocalShellType type,
                               const QString& label)
{
    replace(id, localRuntime(type, label));
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
        return QStringLiteral("local:%1").arg(
            values.value(QStringLiteral("shellType")).toInt());
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

    // 固定分组顺序，避免会话保存顺序改变时侧栏类别来回跳动。
    const std::array kinds{
        TransportKind::LocalShell, TransportKind::Ssh,
        TransportKind::Serial, TransportKind::Telnet,
        TransportKind::Custom};
    for (const TransportKind kind : kinds) {
        QList<const SessionRestoreMetadata*> groupEntries;
        for (const SessionRestoreMetadata& entry : std::as_const(_entries)) {
            if (entry.runtimeSnapshot.transportKind == kind)
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
            if (displayName.isEmpty())
                displayName = sessionName(runtime);

            auto* item = new QTreeWidgetItem(
                group, {displayName, sessionDetail(runtime)});
            item->setData(0, Qt::UserRole,
                          entry->sessionId.toString(QUuid::WithoutBraces));
            item->setToolTip(0, tr("Double-click to reconnect"));
            item->setToolTip(1, tr("Double-click to reconnect"));
        }
    }

    if (_tree->topLevelItemCount() == 0) {
        auto* emptyItem = new QTreeWidgetItem(
            _tree, {tr("No saved sessions yet")});
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
    auto* menu = new ElaMenu(_tree);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setMenuItemHeight(27);
    connect(menu->addElaIconAction(ElaIconType::PenToSquare, tr("Edit")),
            &QAction::triggered, this, [this, item]() { editItem(item); });
    connect(menu->addElaIconAction(ElaIconType::TrashCan, tr("Delete")),
            &QAction::triggered, this, [this, item]() { deleteItem(item); });
    menu->popup(_tree->viewport()->mapToGlobal(position));
}

void SessionPanel::editItem(QTreeWidgetItem* item)
{
    const SessionId id(item->data(0, Qt::UserRole).toString());
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

void SessionPanel::deleteItem(QTreeWidgetItem* item)
{
    const SessionId id(item->data(0, Qt::UserRole).toString());
    const auto it = std::find_if(
        _entries.begin(), _entries.end(), [&id](const auto& entry) {
            return entry.sessionId == id;
        });
    if (it == _entries.end())
        return;

    const QString title = sessionName(it->runtimeSnapshot);
    if (QMessageBox::question(
            this, tr("Delete session"),
            tr("Delete the saved session '%1'?").arg(title),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    if (!it->runtimeSnapshot.credentialRef.isEmpty())
        _credentials->remove(it->runtimeSnapshot.credentialRef);
    _entries.erase(it);
    saveHistory();
    rebuildTree();
}

void SessionPanel::reconnectItem(QTreeWidgetItem* item)
{
    const QUuid id(item->data(0, Qt::UserRole).toString());
    if (id.isNull())
        return;

    const auto it = std::find_if(
        _entries.cbegin(), _entries.cend(), [&id](const auto& entry) {
            return entry.sessionId == id;
        });
    if (it == _entries.cend())
        return;

    const RuntimeConfig& runtime = it->runtimeSnapshot;
    const QVariantMap& values = runtime.transport;
    if (runtime.transportKind == TransportKind::LocalShell) {
        emit localReconnectRequested(
            static_cast<TerminalView::LocalShellType>(
                values.value(QStringLiteral("shellType")).toInt()));
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
            emit reconnectUnavailable(
                tr("The saved SSH credential is unavailable. Create the session again to refresh it."));
            return;
        }
        emit sshReconnectRequested(config);
    }
}

void SessionPanel::retranslateUi()
{
    if (_titleLabel)
        _titleLabel->setText(tr("Quick connections"));
    if (_newSessionButton) {
        _newSessionButton->setText(tr("+  New session"));
        _newSessionButton->setAccessibleName(tr("New session"));
    }
    updateCollapsedUi();
    rebuildTree();
}
