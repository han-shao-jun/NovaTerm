#include "FileResourceSharingDialog.h"
#include "mcp/FileResourceAdapter.h"
#include "mcp/SerialFileTargetAdapter.h"
#include "mcp/McpService.h"
#include "transport/SshTransport.h"
#include "ElaCheckBox.h"
#include "ElaComboBox.h"
#include "ElaLineEdit.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTreeWidget.h"
#include <QFileDialog>
#include <QHeaderView>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QVBoxLayout>

using namespace NovaTerm::Mcp;
FileResourceSharingDialog::FileResourceSharingDialog(Service* service, QString client, QWidget* parent)
    : ElaDialog(parent), _service(service), _client(std::move(client)),
      _identity(service ? service->access().identityVersion(_client) : 0)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
    setObjectName("mcpFileResourceDialog");
    setWindowTitle(tr("文件资源授权"));
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    resize(960, 600);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 8, 16, 16);
    _clientIdentity = new ElaText(this); _clientIdentity->setObjectName("fileResourceClientIdentity");
    _clientIdentity->setTextFormat(Qt::PlainText); _clientIdentity->setWordWrap(true);
    _clientIdentity->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(_clientIdentity);
    auto* help = new ElaText(tr("为当前客户端明确选择本地目录和远端目录的读写权限，仅本次运行有效。"
        "本地读取用于上传源，本地写入用于下载目录；远端读取用于下载源，远端写入用于上传目标。"
        "这些选择与终端权限独立。当前支持受限SFTP上传；串口仅保存ZMODEM发送授权，MCP发送入口仍待后续。授权本身不证明接收端就绪或路径可安全访问。"), this);
    help->setWordWrap(true); layout->addWidget(help);
    _kind = new ElaComboBox(this); _kind->setObjectName("fileResourceKind");
    _kind->addItems({tr("本地目录"), tr("远端 SSH 目录"), tr("串口 ZMODEM 发送")}); layout->addWidget(_kind);
    _session = new ElaComboBox(this); _session->setObjectName("fileResourceSession"); layout->addWidget(_session);
    auto* pathRow = new QHBoxLayout;
    _path = new ElaLineEdit(this); _path->setObjectName("fileResourcePath"); _path->setMaxLength(1024);
    _path->setPlaceholderText(tr("明确选择的目录绝对路径")); pathRow->addWidget(_path, 1);
    _browse = new ElaPushButton(tr("选择本地目录"), this); pathRow->addWidget(_browse); layout->addLayout(pathRow);
    _title = new ElaLineEdit(this); _title->setObjectName("fileResourceTitle"); _title->setMaxLength(256);
    _title->setPlaceholderText(tr("共享名称（会披露给该客户端）")); layout->addWidget(_title);
    auto* actions = new QHBoxLayout;
    _read = new ElaCheckBox(tr("允许读取"), this); _read->setObjectName("fileResourceRead"); actions->addWidget(_read);
    _write = new ElaCheckBox(tr("允许写入"), this); _write->setObjectName("fileResourceWrite"); actions->addWidget(_write);
    _share = new ElaPushButton(tr("共享或更新所填目录"), this); _share->setObjectName("fileResourceShare"); actions->addWidget(_share);
    auto* revoke = new ElaPushButton(tr("撤回选中目录"), this); revoke->setObjectName("fileResourceRevoke"); actions->addWidget(revoke);
    layout->addLayout(actions);
    _roots = new ElaTreeWidget(this); _roots->setObjectName("fileResourceRoots"); _roots->setRootIsDecorated(false);
    _roots->setColumnCount(5); _roots->setHeaderLabels({tr("范围"), tr("本机可见路径"), tr("共享名称"), tr("读取"), tr("写入")});
    _roots->header()->setSectionResizeMode(QHeaderView::ResizeToContents); layout->addWidget(_roots, 1);
    _status = new ElaText(this); _status->setWordWrap(true); layout->addWidget(_status);
    connect(_kind, &QComboBox::currentIndexChanged, this, [this] {
        _path->clear(); _title->clear(); _read->setChecked(false); _write->setChecked(false); refresh();
    });
    connect(_session, &QComboBox::currentIndexChanged, this, [this] { if (!_refreshing) refresh(); });
    connect(_browse, &QPushButton::clicked, this, [this] {
        if (!currentIdentity() || _kind->currentIndex() != 0) return;
        const auto path = QFileDialog::getExistingDirectory(this, tr("选择要共享的本地目录"));
        if (currentIdentity() && !path.isEmpty()) _path->setText(path);
    });
    connect(_share, &QPushButton::clicked, this, [this] { share(); });
    connect(revoke, &QPushButton::clicked, this, [this] {
        const auto* row = _roots->currentItem();
        if (!currentIdentity()) { refresh(); return; }
        if (!row) { _status->setText(tr("请先选择仍有效的目录授权。")); return; }
        const auto id = row->data(0, Qt::UserRole).toString();
        const auto revision = row->data(0, Qt::UserRole + 1).toString();
        const bool removed = row->data(0, Qt::UserRole + 2).toBool()
            ? _service->serialFileTargets().targets().remove(_client, _identity, id, revision)
            : _service->fileResources().resources().remove(_client, _identity, id, revision);
        _status->setText(removed ? tr("已撤回目录授权。") : tr("目录授权已变化，请重新选择。")); refresh();
    });
    if (_service) connect(_service, &Service::changed, this, [this] { refresh(); }, Qt::QueuedConnection);
    refresh();
}
bool FileResourceSharingDialog::currentIdentity() const
{
    return _service && _service->access().enabled() && _identity
        && _service->access().identityVersion(_client) == _identity;
}
void FileResourceSharingDialog::refresh()
{
    if (_refreshing) return;
    const QScopedValueRollback<bool> refreshing(_refreshing, true);
    QString clientLabel = _client;
    if (_service) for (const auto& client : _service->access().clients())
        if (client.id.str() == _client) { clientLabel = client.label; break; }
    setWindowTitle(tr("文件资源授权 — %1").arg(clientLabel));
    _clientIdentity->setText(tr("授权客户端：%1\n客户端 ID：%2").arg(clientLabel, _client));
    const bool current = currentIdentity();
    const bool remote = _kind->currentIndex() == 1;
    const bool serial = _kind->currentIndex() == 2;
    _share->setEnabled(current); _browse->setEnabled(current && !remote && !serial);
    _session->setEnabled(current && (remote || serial));
    _path->setEnabled(current && !serial); _read->setEnabled(current && !serial);
    _write->setEnabled(current); _write->setText(serial ? tr("允许ZMODEM发送") : tr("允许写入"));
    _share->setText(serial ? tr("共享或更新所选串口") : tr("共享或更新所填目录"));
    if (!current) {
        _roots->clear(); _session->clear(); _bindings.clear(); _serialBindings.clear();
        _status->setText(tr("客户端身份或接入状态已变化，请关闭后重新打开授权窗口。")); return;
    }
    const auto selected = _session->currentData().toString();
    const auto oldBinding = _bindings.value(selected);
    const auto oldSerialBinding = _serialBindings.value(selected);
    const auto* currentRoot = _roots->currentItem();
    const auto selectedRoot = currentRoot ? currentRoot->data(0, Qt::UserRole).toString() : QString{};
    const auto selectedRevision = currentRoot ? currentRoot->data(0, Qt::UserRole + 1).toString() : QString{};
    {
        const QSignalBlocker blocker(_session);
        _session->clear(); _bindings.clear(); _serialBindings.clear();
        for (const auto& entry : _service->directory().entries()) {
            if (serial) {
                const auto binding = _service->serialFileTargets().liveBinding(entry.id);
                if (!binding) continue;
                _serialBindings.insert(entry.id, *binding);
                _session->addItem(entry.title + " — " + entry.id.left(8), entry.id);
                continue;
            }
            const auto* ssh = qobject_cast<SshTransport*>(entry.transport.data());
            if (entry.state != SessionState::Running || !ssh || !ssh->isConnected() || entry.targetFingerprint.isEmpty()) continue;
            const SessionBinding binding{NovaTerm::Mcp::SessionId{entry.id}, AttachmentId{entry.attachmentId}, Epoch{entry.epoch},
                Generation{entry.generation}, entry.targetFingerprint};
            _bindings.insert(entry.id, binding);
            _session->addItem(entry.title + " — " + entry.id.left(8), entry.id);
        }
        const bool same = serial
            ? _serialBindings.contains(selected) && SerialFileTargets::sameBinding(oldSerialBinding, _serialBindings.value(selected))
            : _bindings.contains(selected) && FileResources::sameBinding(oldBinding, _bindings.value(selected));
        _session->setCurrentIndex(same ? _session->findData(selected) : -1);
    }
    _roots->clear();
    const auto append = [this, selectedRoot, selectedRevision](const QList<FileRoot>& roots, const QString& scope) {
        for (const auto& root : roots) {
            auto* row = new QTreeWidgetItem(_roots, {scope, root.path, root.title,
                root.access.read ? tr("允许") : tr("关闭"), root.access.write ? tr("允许") : tr("关闭")});
            row->setData(0, Qt::UserRole, root.id); row->setData(0, Qt::UserRole + 1, root.revision);
            if (root.id == selectedRoot && root.revision == selectedRevision) _roots->setCurrentItem(row);
        }
    };
    append(_service->fileResources().resources().local(_client, _identity), tr("本地"));
    const auto id = _session->currentData().toString();
    if (_bindings.contains(id)) append(_service->fileResources().resources().remote(_client, _identity, _bindings.value(id)), tr("当前SSH"));
    if (serial && _serialBindings.contains(id)) {
        const auto listed = _service->serialFileTargets().list(_client, _identity);
        if (!isError(listed)) for (const auto& target : std::get<QList<SerialFileTarget>>(listed)) {
            if (!SerialFileTargets::sameBinding(target.binding, _serialBindings.value(id))) continue;
            auto* row = new QTreeWidgetItem(_roots, {tr("当前串口"), tr("ZMODEM"), target.title, tr("不适用"), tr("允许发送")});
            row->setData(0, Qt::UserRole, target.id); row->setData(0, Qt::UserRole + 1, target.revision);
            row->setData(0, Qt::UserRole + 2, true);
            if (target.id == selectedRoot && target.revision == selectedRevision) _roots->setCurrentItem(row);
        }
    }
}
void FileResourceSharingDialog::share()
{
    if (!currentIdentity()) { refresh(); return; }
    if (_kind->currentIndex() == 2) {
        const auto id = _session->currentData().toString();
        const bool saved = _write->isChecked() && _serialBindings.contains(id)
            && _service->serialFileTargets().share(_client, _identity, _serialBindings.value(id), _title->text().trimmed()).has_value();
        _status->setText(saved ? tr("已保存当前串口的发送授权；MCP串口发送入口尚未开放。")
            : tr("未提交：请选择当前串口、填写共享名称并明确允许ZMODEM发送。"));
        refresh(); return;
    }
    const auto path = _path->text(); const auto title = _title->text().trimmed();
    const FileRootAccess access{_read->isChecked(), _write->isChecked()};
    std::optional<FileRoot> result;
    if (_kind->currentIndex() == 0)
        result = _service->fileResources().resources().shareLocal(_client, _identity, path, title, access);
    else {
        const auto id = _session->currentData().toString();
        if (_bindings.contains(id)) result = _service->fileResources().shareRemote(_client, _identity, _bindings.value(id), path, title, access);
    }
    _status->setText(result ? tr("已保存本次运行的目录授权；尚未传输文件。")
        : tr("未提交：请核对当前SSH连接、绝对目录、共享名称、至少一项读写选择及授权数量。"));
    refresh();
}
