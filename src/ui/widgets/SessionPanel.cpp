/**
 * @file   SessionPanel.cpp
 * @brief  会话面板实现：历史会话树、重连与编辑。
 *
 * 通过 SessionStore 持久化会话历史，QTreeWidget 展示。右键菜单支持重连、
 * 编辑、删除。折叠/展开按钮在窗口宽度不足时隐藏树。
 */
#include "SessionPanel.h"

#include "ElaIconButton.h"
#include "ElaMenu.h"
#include "credential/CredentialStore.h"
#include "service/LanguageManager.h"
#include "session/SessionStore.h"

#include <QDir>
#include <QDebug>
#include <QMessageBox>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
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
    return QStringLiteral("shel");
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

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    _tree = new QTreeWidget(this);
    _tree->setColumnCount(1);
    _tree->setHeaderHidden(true);
    _tree->setAnimated(false);
    _tree->setIndentation(0);
    _tree->setRootIsDecorated(false);
    _tree->setUniformRowHeights(true);
    _tree->setContextMenuPolicy(Qt::CustomContextMenu);
    rootLayout->addWidget(_tree, 1);

    _toggleButton = new ElaIconButton(
        ElaIconType::AngleLeft, 14, 32, 32, this);
    _toggleButton->raise();

    connect(_toggleButton, &QPushButton::clicked, this,
            [this]() { setExpanded(!_expanded); });
    connect(_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) { reconnectItem(item); });
    connect(_tree, &QWidget::customContextMenuRequested, this,
            &SessionPanel::showItemContextMenu);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    setExpanded(true);
    rebuildTree();
}

SessionPanel::~SessionPanel() = default;

void SessionPanel::setExpanded(bool expanded)
{
    if (_expanded && !expanded && width() >= 160)
        _expandedWidth = width();

    _expanded = expanded;
    _tree->setVisible(expanded);
    _toggleButton->setAwesome(expanded ? ElaIconType::AngleLeft
                                       : ElaIconType::AngleRight);
    _toggleButton->setAccessibleName(
        expanded ? tr("Collapse sessions") : tr("Expand sessions"));
    _toggleButton->setToolTip(
        expanded ? tr("Collapse sessions") : tr("Expand sessions"));
    if (expanded) {
        setMaximumWidth(QWIDGETSIZE_MAX);
        setMinimumWidth(160);
    } else {
        setMinimumWidth(44);
        setMaximumWidth(44);
    }
    updateGeometry();
    repositionToggleButton();
    emit panelWidthChangeRequested(expanded ? _expandedWidth : 44);
}

void SessionPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (_expanded && isVisible() && event->size().width() >= 160)
        _expandedWidth = event->size().width();
    repositionToggleButton();
}

void SessionPanel::repositionToggleButton()
{
    if (!_toggleButton)
        return;

    const int rightMargin = 4;
    _toggleButton->move(width() - _toggleButton->width() - rightMargin,
                        (height() - _toggleButton->height()) / 2);
    _toggleButton->raise();
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
    for (const SessionRestoreMetadata& entry : std::as_const(_entries)) {
        const RuntimeConfig& runtime = entry.runtimeSnapshot;
        auto* item = new QTreeWidgetItem(_tree, {sessionName(runtime)});
        item->setData(0, Qt::UserRole,
                      entry.sessionId.toString(QUuid::WithoutBraces));
        item->setToolTip(0, tr("Double-click to reconnect"));
    }
}

void SessionPanel::showItemContextMenu(const QPoint& position)
{
    QTreeWidgetItem* const item = _tree->itemAt(position);
    if (!item)
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
    setExpanded(_expanded);
    rebuildTree();
}
