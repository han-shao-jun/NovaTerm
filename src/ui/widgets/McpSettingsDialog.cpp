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
#include <QSignalBlocker>
#include <QVBoxLayout>

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
        "Command access is separate and only allows fixed diagnostics: SSH commands use the trusted connected account, "
        "while Windows local commands run in an isolated helper. Deletion, credentials, privilege elevation and arbitrary scripts are prohibited."), this);
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
    auto* rotate = new ElaPushButton(tr("Rotate access token"), this);
    actions->addWidget(copy);
    actions->addWidget(rotate);
    actions->addStretch();
    layout->addLayout(actions);
    _sessions = new ElaTreeWidget(this);
    _sessions->setColumnCount(4);
    _sessions->setHeaderLabels({tr("Session"), tr("State"), tr("Read output"), tr("Authorized diagnostics")});
    _sessions->setRootIsDecorated(true);
    _sessions->setItemHeight(30);
    _sessions->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
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
    connect(rotate, &QPushButton::clicked, this, [this] {
        if (_service && !_service->access().rotateToken(_clients->currentData().toString())) reportFailure();
    });
    connect(_sessions, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (_refreshing || !_service || column < 2) return;
        auto* root = item->parent() ? item->parent() : item;
        const auto id = root->data(0, Qt::UserRole).toString();
        const bool read = root->checkState(2) == Qt::Checked;
        if (column == 3 && !item->parent()) {
            const QSignalBlocker blocker(_sessions);
            const auto state = root->checkState(3) == Qt::Checked ? Qt::Checked : Qt::Unchecked;
            for (int i = 0; i < root->childCount(); ++i) root->child(i)->setCheckState(3, state);
        }
        const auto entry = _service->directory().find(id);
        QSet<QString> allowed;
        if (read) for (int i = 0; i < root->childCount(); ++i) {
            if (root->child(i)->checkState(3) == Qt::Checked)
                allowed.insert(root->child(i)->data(0, Qt::UserRole).toString());
        }
        if (entry) _service->access().setGrant(_clients->currentData().toString(), *entry, read, allowed);
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
            SessionDirectory::stateName(entry.state), {}, {}});
        item->setData(0, Qt::UserRole, entry.id);
        item->setCheckState(2, _service->access().canRead(clientId, entry) ? Qt::Checked : Qt::Unchecked);
        const auto allowed = _service->access().commands(clientId, entry);
        const auto profile = entry.session
            ? entry.session->commandFacade()->profile() : CommandPlatformProfile{};
        const auto catalog = NovaTerm::Mcp::CommandPolicy::catalog(profile);
        item->setCheckState(3, allowed.isEmpty() ? Qt::Unchecked
            : allowed.size() == catalog.size() ? Qt::Checked : Qt::PartiallyChecked);
        item->setToolTip(3, entry.kind == TransportKind::LocalShell
            ? tr("Fixed diagnostics run in an isolated local helper and do not write to the current shell.")
            : tr("Only enable for a trusted Linux/POSIX SSH server. Fixed diagnostics run as its connected user."));
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
            child->setCheckState(3, allowed.contains(command.id) ? Qt::Checked : Qt::Unchecked);
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
