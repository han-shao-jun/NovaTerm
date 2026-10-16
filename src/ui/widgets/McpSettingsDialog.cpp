/** @file McpSettingsDialog.cpp
 *  @brief 用户可导出令牌或解除目标暂停；这些操作不暴露给 MCP。
 */
#include "McpSettingsDialog.h"
#include "mcp/McpService.h"
#include "mcp/CommandPolicy.h"
#include "session/SessionCommandFacade.h"
#include "session/SessionTerminalController.h"
#include "ElaCheckBox.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaPlainTextEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTreeWidget.h"
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {

/* 本文件的列号是裸数字：Qt lupdate 的 C++ 解析器靠花括号嵌套推断 tr() 的
 * context，在此处加任何类型/枚举声明都会让 McpSettingsDialog 的既有 33 条
 * 译文被误判成新条目而清空。列号与授权字段的对应关系由
 * McpTests::settingsDialogSeparatesReadAndScriptPermission 逐列往返断言守住。 */

class PermissionCellDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option,
                     const QModelIndex& index) override
    {
        if (index.column() >= 2
            && index.column() <= 7
            && index.flags().testFlag(Qt::ItemIsEnabled)
            && index.flags().testFlag(Qt::ItemIsUserCheckable)
            && model->data(index, Qt::CheckStateRole).isValid()
            && event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton
                && option.rect.contains(mouse->position().toPoint())) {
                const auto state = model->data(index, Qt::CheckStateRole).toInt();
                return model->setData(index,
                    state == Qt::Checked ? Qt::Unchecked : Qt::Checked,
                    Qt::CheckStateRole);
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

} // namespace

McpSettingsDialog::McpSettingsDialog(NovaTerm::Mcp::Service* service, QWidget* parent)
    : ElaDialog(parent), _service(service)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("AI MCP access"));
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    resize(1180, 800);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setSpacing(10);
    _enabled = new ElaCheckBox(tr("Enable local MCP access"), this);
    layout->addWidget(_enabled);
    auto* explanation = new ElaText(tr("Share only the sessions you select. Terminal output may contain sensitive information. "
        "Fixed diagnostics stay separate from interactive terminal input. Shared sessions can run ordinary commands; potentially destructive commands and every script require confirmation in the MCP client. "
        "Script contents are written to the selected host path and are never sent to the terminal UI. Risk checks are best-effort, not a sandbox."), this);
    explanation->setWordWrap(true);
    explanation->setTextPixelSize(12);
    layout->addWidget(explanation);
    auto* interactionHelp = new ElaText(tr("允许执行命令授予当前连接的完整终端输入权限，不再逐条确认。旧命令接口的诊断、脚本和交互权限保持独立。托管提权必须另外授权；保存密码不会启用提权，可信认证适配器尚不可用。"), this);
    interactionHelp->setWordWrap(true);
    interactionHelp->setTextPixelSize(12);
    layout->addWidget(interactionHelp);
    _status = new ElaText(this);
    _status->setWordWrap(true);
    _status->setTextPixelSize(12);
    layout->addWidget(_status);
    auto* clients = new QHBoxLayout;
    _clients = new ElaComboBox(this);
    _clients->setMinimumWidth(180);
    clients->addWidget(_clients, 1);
    _label = new ElaLineEdit(this);
    _label->setPlaceholderText(tr("New client label"));
    _label->setMaxLength(80);
    clients->addWidget(_label, 1);
    auto* add = new ElaPushButton(tr("Add client"), this);
    auto* remove = new ElaPushButton(tr("Remove client"), this);
    clients->addWidget(add);
    clients->addWidget(remove);
    layout->addLayout(clients);
    auto* actions = new QHBoxLayout;
    auto* copy = new ElaPushButton(tr("Copy MCP configuration"), this);
    auto* copyForCcSwitch = new ElaPushButton(tr("Copy for CC Switch"), this);
    auto* rotate = new ElaPushButton(tr("Rotate access token"), this);
    auto* takeOver = new ElaPushButton(tr("接管选中会话"), this);
    actions->addWidget(copy);
    actions->addWidget(copyForCcSwitch);
    actions->addWidget(rotate);
    actions->addWidget(takeOver);
    actions->addStretch();
    layout->addLayout(actions);
    _sessions = new ElaTreeWidget(this);
    _sessions->setColumnCount(10);
    _sessions->setHeaderLabels({tr("Session"), tr("State"), tr("Read output"),
        tr("Fixed diagnostics"), tr("Script tasks"), tr("Interactive commands"),
        tr("允许执行命令"), tr("允许托管提权"), tr("提权凭据"), tr("当前操作")});
    // 授权列整格均可点击，避免仅命中小尺寸复选框时才能切换状态。
    auto* permissionDelegate = new PermissionCellDelegate(_sessions);
    for (int column = 2; column < 8; ++column)
        _sessions->setItemDelegateForColumn(column, permissionDelegate);
    _sessions->setRootIsDecorated(true);
    _sessions->setItemHeight(30);
    _sessions->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 10; ++column)
        _sessions->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(_sessions, 2);
    auto* legacyPermissions = new ElaCheckBox(tr("显示旧命令接口详细权限"), this);
    legacyPermissions->setChecked(true);
    connect(legacyPermissions, &QCheckBox::toggled, this, [this](bool visible) {
        for (int column = 3; column <= 5; ++column) _sessions->setColumnHidden(column, !visible);
    });
    layout->addWidget(legacyPermissions);
    _records = new ElaComboBox(this);
    layout->addWidget(_records);
    _details = new ElaPlainTextEdit(this);
    _details->setReadOnly(true);
    _details->setMaximumBlockCount(1024);
    _details->setPlaceholderText(tr("Command results appear here. A disconnected or timed-out command may still be running remotely."));
    layout->addWidget(_details, 1);
    _targets = new ElaComboBox(this);
    layout->addWidget(_targets);
    _confirmedStopped = new ElaCheckBox(tr("I have checked that the remote command has stopped"), this);
    auto* acknowledge = new ElaPushButton(tr("Release selected target"), this);
    auto* release = new QHBoxLayout;
    release->addWidget(_confirmedStopped, 1);
    release->addWidget(acknowledge);
    layout->addLayout(release);
    connect(_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!_refreshing && _service && !_service->access().setEnabled(enabled)) { refresh(); reportFailure(); }
    });
    connect(_clients, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { if (!_refreshing) refresh(); });
    connect(add, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto id = _service->access().addClient(_label->text());
        if (id.isEmpty()) { reportFailure(); return; }
        _label->clear();
        refresh();
        _clients->setCurrentIndex(_clients->findData(id));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (_service && !_service->access().removeClient(_clients->currentData().toString())) reportFailure();
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto configuration = _service->clientConfiguration(_clients->currentData().toString());
        if (configuration.isEmpty()) { reportFailure(); return; }
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(configuration).toJson()));
        _status->setText(tr("Configuration copied. It contains an access token; keep it private."));
    });
    connect(copyForCcSwitch, &QPushButton::clicked, this, [this] {
        if (!_service) return;
        const auto configuration = _service->ccSwitchConfiguration(_clients->currentData().toString());
        if (configuration.isEmpty()) { reportFailure(); return; }
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(configuration).toJson()));
        _status->setText(tr("CC Switch configuration copied. It contains an access token; keep it private."));
    });
    connect(rotate, &QPushButton::clicked, this, [this] {
        if (_service && !_service->access().rotateToken(_clients->currentData().toString())) reportFailure();
    });
    connect(takeOver, &QPushButton::clicked, this, [this] {
        if (!_service || !_sessions->currentItem()) return;
        auto* item = _sessions->currentItem();
        if (item->parent()) item = item->parent();
        const auto entry = _service->directory().find(item->data(0, Qt::UserRole).toString());
        if (entry && entry->session)
            entry->session->terminalController()->release({}, QStringLiteral("preempted"));
        refresh();
    });
    connect(_sessions, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (_refreshing || !_service || column < 2
            || column > 7) {
            return;
        }
        auto* root = item->parent() ? item->parent() : item;
        const auto id = root->data(0, Qt::UserRole).toString();
        const auto entry = _service->directory().find(id);
        if (!entry) return;
        if (column >= 6) {
            const QSignalBlocker blocker(_sessions);
            if (column == 6 && root->checkState(6) == Qt::Checked)
                root->setCheckState(2, Qt::Checked);
            if (root->checkState(6) != Qt::Checked) root->setCheckState(7, Qt::Unchecked);
        }
        const bool read = root->checkState(2) == Qt::Checked;
        if (column == 3 && !item->parent()) {
            const QSignalBlocker blocker(_sessions);
            const auto state = root->checkState(3) == Qt::Checked
                ? Qt::Checked : Qt::Unchecked;
            for (int i = 0; i < root->childCount(); ++i)
                root->child(i)->setCheckState(3, state);
        } else if (column == 3 && item->parent()) {
            const QSignalBlocker blocker(_sessions);
            int checked = 0;
            for (int i = 0; i < root->childCount(); ++i)
                checked += root->child(i)->checkState(3) == Qt::Checked;
            root->setCheckState(3, checked == 0 ? Qt::Unchecked
                : checked == root->childCount() ? Qt::Checked : Qt::PartiallyChecked);
        }
        if (!read) {
            const QSignalBlocker blocker(_sessions);
            root->setCheckState(3, Qt::Unchecked);
            root->setCheckState(4, Qt::Unchecked);
            root->setCheckState(5, Qt::Unchecked);
            root->setCheckState(6, Qt::Unchecked);
            root->setCheckState(7, Qt::Unchecked);
            for (int i = 0; i < root->childCount(); ++i)
                root->child(i)->setCheckState(3, Qt::Unchecked);
        }
        QSet<QString> allowed;
        if (read) for (int i = 0; i < root->childCount(); ++i) {
            if (root->child(i)->checkState(3) == Qt::Checked)
                allowed.insert(root->child(i)->data(0, Qt::UserRole).toString());
        }
        const auto client = _clients->currentData().toString();
        const bool grantSaved = _service->access().setGrant(client, *entry,
            read, allowed, root->checkState(5) == Qt::Checked,
            false, root->checkState(4) == Qt::Checked);
        if (!grantSaved || (column >= 6 && read
            && !_service->access().setTerminalGrant(client, *entry,
                root->checkState(6) == Qt::Checked, root->checkState(7) == Qt::Checked))) {
            refresh();
            reportFailure();
        }
    });
    connect(_records, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { if (!_refreshing) showRecord(); });
    connect(_targets, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!_refreshing) _confirmedStopped->setChecked(false);
    });
    connect(_confirmedStopped, &QCheckBox::toggled, this, [this](bool checked) {
        _confirmedFingerprint = checked ? _targets->currentData().toString() : QString{};
        _confirmedExecutionId = checked
            ? _targets->currentData(Qt::UserRole + 1).toString() : QString{};
    });
    connect(acknowledge, &QPushButton::clicked, this, [this] {
        if (!_confirmedStopped->isChecked() || !_service) return;
        const bool released = !_confirmedFingerprint.isEmpty()
            && !_confirmedExecutionId.isEmpty()
            && _confirmedFingerprint == _targets->currentData().toString()
            && _confirmedExecutionId == _targets->currentData(Qt::UserRole + 1).toString()
            && _service->acknowledgeTarget(_confirmedFingerprint, _confirmedExecutionId);
        _confirmedStopped->setChecked(false);
        if (!released) reportFailure();
    });
    if (_service) connect(_service, &NovaTerm::Mcp::Service::changed,
        this, &McpSettingsDialog::refresh, Qt::QueuedConnection);
    refresh();
}

void McpSettingsDialog::editElevationCredentials(const QString& sessionId)
{
    if (!_service) return;
    const auto entry = _service->directory().find(sessionId);
    if (!entry || !entry->session) return;
    const auto binding = *entry;
    const auto matchesBinding = [binding](const SessionDirectory::Entry& current) {
        return binding.session && current.session == binding.session
            && current.id == binding.id && current.attachmentId == binding.attachmentId
            && current.epoch == binding.epoch && current.generation == binding.generation
            && current.targetFingerprint == binding.targetFingerprint
            && current.transport == binding.transport && current.kind == binding.kind
            && current.state == SessionState::Running;
    };
    auto* dialog = new ElaDialog(this);
    dialog->setObjectName(QStringLiteral("mcpElevationCredentialsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("提权凭据 — %1").arg(entry->title));
    dialog->setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    dialog->resize(560, 330);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 8, 16, 16);
    auto* explanation = new ElaText(tr("sudo 认证密码和 su root 密码分别保存，不使用 SSH 登录密码。保存后不回填。只有明确授权且可信适配器可用时才能托管认证；当前适配器不可用。撤销授权不会退出已经获得的 root Shell。"), dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* status = new ElaText(_service->access().elevationCredentialsPersistent()
        ? tr("存储：系统凭据库。密码仅在本机设置，不对 MCP 客户端开放。")
        : tr("存储：仅本次运行。密码仅在本机设置，不对 MCP 客户端开放。"), dialog);
    status->setWordWrap(true);
    layout->addWidget(status);
    for (const auto& method : {QStringLiteral("sudo"), QStringLiteral("su")}) {
        auto* title = new ElaText(method == "sudo" ? tr("sudo 认证密码") : tr("su root 密码"), dialog);
        layout->addWidget(title);
        auto* row = new QHBoxLayout;
        auto* password = new ElaLineEdit(dialog);
        password->setObjectName(QStringLiteral("mcpElevationPassword_") + method);
        password->setEchoMode(QLineEdit::Password);
        password->setMaxLength(1024);
        password->setPlaceholderText(tr("输入新密码；留空不会删除"));
        auto* save = new ElaPushButton(tr("保存 / 替换"), dialog);
        auto* remove = new ElaPushButton(tr("删除"), dialog);
        save->setObjectName(QStringLiteral("mcpElevationSave_") + method);
        remove->setObjectName(QStringLiteral("mcpElevationRemove_") + method);
        remove->setEnabled(_service->access().hasElevationCredential(*entry, method));
        row->addWidget(password, 1);
        row->addWidget(save);
        row->addWidget(remove);
        layout->addLayout(row);
        const auto update = [this, sessionId, binding, matchesBinding, method, password, save, remove, status,
                             guard = QPointer<ElaDialog>(dialog)](bool deleting) {
            if (!guard || !_service) return;
            const auto current = _service->directory().find(sessionId);
            if (!current || !matchesBinding(*current)) {
                password->clear();
                save->setEnabled(false);
                remove->setEnabled(false);
                status->setText(tr("会话连接或目标已变化，未提交凭据操作。请关闭并重新打开设置。"));
                return;
            }
            QByteArray secret = deleting ? QByteArray{} : password->text().toUtf8();
            password->clear();
            save->setEnabled(false);
            remove->setEnabled(false);
            status->setText(tr("正在访问本机凭据库；可以关闭此窗口。"));
            const auto completed = [guard, service = _service, sessionId, matchesBinding, method,
                                    save, remove, status](bool ok) {
                if (!guard || !service) return;
                const auto active = service->directory().find(sessionId);
                if (!active || !matchesBinding(*active)) {
                    save->setEnabled(false);
                    remove->setEnabled(false);
                    status->setText(QObject::tr("会话连接或目标已变化；凭据操作针对打开窗口时的原目标。请关闭并重新打开设置。"));
                    return;
                }
                save->setEnabled(true);
                remove->setEnabled(service->access().hasElevationCredential(*active, method));
                status->setText(ok
                    ? (service->access().elevationCredentialsPersistent()
                        ? QObject::tr("已更新系统凭据库。托管提权授权已撤销，请按需重新授权。")
                        : QObject::tr("已更新，仅本次运行保存。托管提权授权已撤销，请按需重新授权。"))
                    : QObject::tr("凭据操作失败；请检查会话身份、密码是否为空及凭据库权限。"));
            };
            if (!_service->access().configureElevationCredentialAsync(
                    binding, method, std::move(secret), deleting, completed)) completed(false);
        };
        connect(save, &QPushButton::clicked, dialog, [update] { update(false); });
        connect(remove, &QPushButton::clicked, dialog, [update] { update(true); });
    }
    dialog->open();
}

void McpSettingsDialog::reportFailure()
{
    // AccessStore 把最近一次保存失败的原因留在 lastSaveFailure()，这里取用后
    // 立刻清空，避免上一次失败的原因被下一次无关失败误读。目录未加固与被别的
    // 实例持锁占用是两种最常见也最难自察的成因，处置完全不同，因此各自给一条
    // 可操作的提示。
    if (!_service) return;
    using Failure = NovaTerm::Mcp::AccessStore::SaveFailure;
    const Failure reason = _service->access().lastSaveFailure();
    _service->access().clearSaveFailure();
    if (reason == Failure::Locked) {
        _status->setText(tr("Cannot update MCP access settings: another NovaTerm instance is holding the shared access file. Close that instance and try again."));
        return;
    }
    if (reason == Failure::Directory) {
        _status->setText(tr("Cannot update MCP access settings: the MCP state directory is not private to this user. Fix its permissions and owner, then try again."));
        return;
    }
    _status->setText(tr("Operation failed. Check the selected client, storage permissions, and active commands."));
}
void McpSettingsDialog::refresh()
{
    if (_refreshing || !_service) return;
    _refreshing = true;
    const QSignalBlocker clientsBlocker(_clients), sessionsBlocker(_sessions), enabledBlocker(_enabled);
    _enabled->setChecked(_service->access().enabled());
    QString state = _service->status();
    if (state == "Disabled") state = tr("Disabled");
    else if (state == "Listening") state = tr("Listening");
    else if (state == "Starting") state = tr("Starting");
    QString statusText = tr("Status: %1; instance: %2").arg(state, _service->instanceId());
    if (!_service->httpEndpoint().isEmpty()) statusText += '\n' + _service->httpEndpoint();
    _status->setText(statusText);
    const auto selected = _clients->currentData().toString();
    _clients->clear();
    for (const auto& client : _service->access().clients())
        _clients->addItem(client.label + (client.persistent ? QString() : tr(" (this run only)")), client.id);
    if (_clients->findData(selected) >= 0) _clients->setCurrentIndex(_clients->findData(selected));
    const auto clientId = _clients->currentData().toString();
    // 本函数每次都重建整棵树，而根项默认折叠：先按会话 ID 记下当前展开的根项，
    // 重建后原样恢复。否则勾选任意一条授权（itemChanged → setGrant → changed →
    // 排队的 refresh）都会把用户刚展开的层级收起来，每次授权都要重新展开。
    QSet<QString> expandedSessions;
    for (int i = 0; i < _sessions->topLevelItemCount(); ++i) {
        const auto* root = _sessions->topLevelItem(i);
        const auto id = root->data(0, Qt::UserRole).toString();
        if (root->isExpanded() && !id.isEmpty())
            expandedSessions.insert(id);
    }
    _sessions->clear();
    for (const auto& entry : _service->directory().entries()) {
        auto* item = new QTreeWidgetItem(_sessions, {entry.title.isEmpty() ? entry.id : entry.title,
            SessionDirectory::stateName(entry.state), {}, {}, {}, {}});
        item->setData(0, Qt::UserRole, entry.id);
        item->setToolTip(0, entry.title.isEmpty() ? entry.id : entry.title + '\n' + entry.id);
        if (expandedSessions.contains(entry.id)) item->setExpanded(true);
        item->setCheckState(2,
            _service->access().canRead(clientId, entry) ? Qt::Checked : Qt::Unchecked);
        const auto allowed = _service->access().commands(clientId, entry);
        const auto profile = entry.session
            ? entry.session->commandFacade()->profile() : CommandPlatformProfile{};
        const auto catalog = NovaTerm::Mcp::CommandPolicy::catalog(profile);
        item->setCheckState(3, allowed.isEmpty() ? Qt::Unchecked
            : allowed.size() == catalog.size() ? Qt::Checked : Qt::PartiallyChecked);
        item->setToolTip(3, entry.kind == TransportKind::LocalShell
            ? tr("Fixed diagnostics run in an isolated local helper and do not write to the current shell.")
            : tr("Only enable for a trusted Linux/POSIX SSH server. Fixed diagnostics run as its connected user."));
        item->setCheckState(4,
            _service->access().canRunScriptTask(clientId, entry) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(4, tr("Allows LocalShell/SSH script tasks. Each script still requires MCP-client confirmation; its body is written to the requested host path and is not shown in the terminal UI."));
        item->setCheckState(5,
            _service->access().canRunCommand(clientId, entry) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(5, tr("Allows the MCP client to type commands into this session's current terminal. Ordinary low-risk commands run without confirmation; anything potentially destructive or unclassifiable still requires confirmation in the MCP client. Off by default: sharing read output does not imply permission to type."));
        item->setCheckState(6,
            _service->access().canControlTerminal(clientId, entry) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(6, tr("允许当前客户端在本次连接输入文本、按键并执行命令。重连、撤权或目标变化后需要重新授权。"));
        item->setCheckState(7,
            _service->access().canUseElevation(clientId, entry) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(7, tr("须先允许执行命令。此授权不会配置密码或启用认证适配器；当前没有通过验收的 sudo/su 适配器，提权请求将返回不可用。串口和 Telnet 不支持托管提权。"));
        const bool sudoSaved = _service->access().hasElevationCredential(entry, "sudo");
        const bool suSaved = _service->access().hasElevationCredential(entry, "su");
        auto* credentials = new ElaPushButton(sudoSaved && suSaved ? tr("sudo / su")
            : sudoSaved ? tr("sudo") : suSaved ? tr("su") : tr("未配置"), _sessions);
        credentials->setEnabled(entry.state == SessionState::Running
            && !entry.targetFingerprint.isEmpty()
            && (entry.kind == TransportKind::LocalShell || entry.kind == TransportKind::Ssh));
        credentials->setToolTip(_service->access().elevationCredentialsPersistent()
            ? tr("密码保存在系统凭据库；保存不代表授权使用，也不启用认证适配器。")
            : tr("密码仅本次运行保留；保存不代表授权使用，也不启用认证适配器。"));
        connect(credentials, &QPushButton::clicked, this,
            [this, id = entry.id] { editElevationCredentials(id); });
        _sessions->setItemWidget(item, 8, credentials);
        if (entry.session) {
            auto* controller = entry.session->terminalController();
            const auto current = controller->observation(clientId);
            const auto command = current.value("command").toObject();
            QString operation = controller->owner().isEmpty() ? tr("空闲") : tr("Agent 操作");
            for (const auto& client : _service->access().clients())
                if (client.id == controller->owner()) operation += " / " + client.label;
            const auto controlState = current.value("control").toObject().value("state").toString();
            if (controlState == "preempted") operation = tr("用户接管");
            if (!command.value("state").toString().isEmpty())
                operation += " / " + command.value("state").toString();
            item->setText(9, operation);
        }
        for (const auto& command : catalog) {
            QString title = command.title;
            if (command.id == "system.identity") title = tr("System identity");
            else if (command.id == "system.uptime") title = tr("Uptime and load");
            else if (command.id == "memory.summary") title = tr("Memory summary");
            else if (command.id == "filesystem.usage") title = tr("Filesystem capacity");
            auto* child = new QTreeWidgetItem(item, {title});
            child->setData(0, Qt::UserRole, command.id);
            child->setToolTip(0, entry.kind == TransportKind::LocalShell
                ? QStringLiteral("novaterm-local-diag ") + command.id
                : QString::fromUtf8(command.command));
            child->setCheckState(3,
                allowed.contains(command.id) ? Qt::Checked : Qt::Unchecked);
        }
    }
    _sessions->setEnabled(!clientId.isEmpty() && _service->access().enabled());
    const auto previousRecord = _records->currentData().toString();
    _records->clear();
    _recordData.clear();
    for (const auto& value : _service->executionRecords()) {
        const auto record = value.toObject();
        const auto id = record.value("executionId").toString();
        _records->addItem(record.value("commandId").toString() + " / " + id.left(8), id);
        _recordData.insert(id, record);
    }
    if (_records->findData(previousRecord) >= 0) _records->setCurrentIndex(_records->findData(previousRecord));
    const auto previousTarget = _targets->currentData().toString();
    const QSignalBlocker targetsBlocker(_targets);
    _targets->clear();
    const auto targets = _service->protectedTargets();
    for (auto it = targets.begin(); it != targets.end(); ++it) {
        const auto executionId = it.value().toObject().value("executionId").toString();
        _targets->addItem(tr("Protected target %1 — execution %2")
            .arg(it.key().left(12), executionId.left(8)), it.key());
        _targets->setItemData(_targets->count() - 1, executionId, Qt::UserRole + 1);
    }
    const auto previousTargetIndex = _targets->findData(previousTarget);
    if (previousTargetIndex >= 0) _targets->setCurrentIndex(previousTargetIndex);
    if (_confirmedFingerprint != _targets->currentData().toString()
        || _confirmedExecutionId != _targets->currentData(Qt::UserRole + 1).toString()) {
        _confirmedStopped->setChecked(false);
    }
    _refreshing = false;
    showRecord();
}
void McpSettingsDialog::showRecord()
{
    const auto record = _recordData.value(_records->currentData().toString());
    if (record.isEmpty()) { _details->clear(); return; }
    if (!record.value("complete").toBool()) { _details->setPlainText(tr("Command is running.")); return; }
    const auto payload = record.value("result").toObject();
    const auto details = payload.value("ok").toBool() ? payload.value("data").toObject()
        : payload.value("error").toObject().value("details").toObject();
    _details->setPlainText(tr("Status: %1\nExit code: %2\nTermination confirmed: %3\n\n%4\n%5")
        .arg(details.value("status").toString(), details.value("exitCode").isNull()
            ? tr("Unknown") : QString::number(details.value("exitCode").toInt()),
            details.value("terminationConfirmed").toBool() ? tr("Yes") : tr("No"),
            details.value("stdout").toString(), details.value("stderr").toString()));
}
