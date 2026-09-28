/** @file McpSettingsDialog.cpp
 *  @brief 用户可导出令牌或解除目标暂停；这些操作不暴露给 MCP。
 */
#include "McpSettingsDialog.h"
#include "mcp/McpService.h"
#include "mcp/CommandPolicy.h"
#include "session/SessionCommandFacade.h"
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
            && index.column() <= 5
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
    resize(940, 760);
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
    actions->addWidget(copy);
    actions->addWidget(copyForCcSwitch);
    actions->addWidget(rotate);
    actions->addStretch();
    layout->addLayout(actions);
    _sessions = new ElaTreeWidget(this);
    _sessions->setColumnCount(6);
    _sessions->setHeaderLabels({tr("Session"), tr("State"), tr("Read output"),
        tr("Fixed diagnostics"), tr("Script tasks"), tr("Interactive commands")});
    // 授权列整格均可点击，避免仅命中小尺寸复选框时才能切换状态。
    auto* permissionDelegate = new PermissionCellDelegate(_sessions);
    for (int column = 2; column < 6; ++column)
        _sessions->setItemDelegateForColumn(column, permissionDelegate);
    _sessions->setRootIsDecorated(true);
    _sessions->setItemHeight(30);
    _sessions->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 6; ++column)
        _sessions->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(_sessions, 2);
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
    connect(_sessions, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (_refreshing || !_service || column < 2
            || column > 5) {
            return;
        }
        auto* root = item->parent() ? item->parent() : item;
        const auto id = root->data(0, Qt::UserRole).toString();
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
            for (int i = 0; i < root->childCount(); ++i)
                root->child(i)->setCheckState(3, Qt::Unchecked);
        }
        const auto entry = _service->directory().find(id);
        QSet<QString> allowed;
        if (read) for (int i = 0; i < root->childCount(); ++i) {
            if (root->child(i)->checkState(3) == Qt::Checked)
                allowed.insert(root->child(i)->data(0, Qt::UserRole).toString());
        }
        if (entry) _service->access().setGrant(_clients->currentData().toString(), *entry,
            read, allowed, root->checkState(5) == Qt::Checked,
            false, root->checkState(4) == Qt::Checked);
    });
    connect(_records, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { if (!_refreshing) showRecord(); });
    connect(acknowledge, &QPushButton::clicked, this, [this] {
        if (!_confirmedStopped->isChecked() || !_service) return;
        const bool released = _service->acknowledgeTarget(_targets->currentData().toString());
        _confirmedStopped->setChecked(false);
        if (!released) reportFailure();
    });
    if (_service) connect(_service, &NovaTerm::Mcp::Service::changed,
        this, &McpSettingsDialog::refresh, Qt::QueuedConnection);
    refresh();
}

void McpSettingsDialog::reportFailure()
{
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
    _status->setText(tr("Status: %1; instance: %2").arg(state, _service->instanceId()));
    const auto selected = _clients->currentData().toString();
    _clients->clear();
    for (const auto& client : _service->access().clients())
        _clients->addItem(client.label + (client.persistent ? QString() : tr(" (this run only)")), client.id);
    if (_clients->findData(selected) >= 0) _clients->setCurrentIndex(_clients->findData(selected));
    const auto clientId = _clients->currentData().toString();
    _sessions->clear();
    for (const auto& entry : _service->directory().entries()) {
        auto* item = new QTreeWidgetItem(_sessions, {entry.title.isEmpty() ? entry.id : entry.title,
            SessionDirectory::stateName(entry.state), {}, {}, {}, {}});
        item->setData(0, Qt::UserRole, entry.id);
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
    _targets->clear();
    const auto targets = _service->protectedTargets();
    for (auto it = targets.begin(); it != targets.end(); ++it)
        _targets->addItem(tr("Protected target %1 — execution %2")
            .arg(it.key().left(12), it.value().toObject().value("executionId").toString().left(8)), it.key());
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
