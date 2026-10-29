#include "RemoteDirectoryDialog.h"
#include "mcp/contracts/Budget.h"
#include <ElaLineEdit.h>
#include <ElaPushButton.h>
#include <ElaText.h>
#include <ElaTreeWidget.h>
#include <QDir>
#include <QHeaderView>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {
bool validPath(const QString& path)
{
    const auto bytes = path.toUtf8();
    if (!path.startsWith('/') || path.startsWith("//") || bytes.size() > 4096
        || QString::fromUtf8(bytes) != path || QDir::cleanPath(path) != path) return false;
    for (const auto ch : path) if (ch.unicode() < 32 || ch.unicode() == 127) return false;
    return true;
}
bool shareablePath(const QString& path)
{
    return path != "/" && path.toUtf8().size() <= NovaTerm::Mcp::Budget::MaxFileRootPathBytes;
}
}

RemoteDirectoryDialog::RemoteDirectoryDialog(QString initialPath, List list,
    std::function<bool()> current, QWidget* parent)
    : ElaDialog(parent), _list(std::move(list)), _current(std::move(current))
{
    setObjectName("remoteDirectoryDialog");
    setWindowTitle(tr("选择远端目录"));
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    setIsStayTop(false); setIsDefaultClosed(false);
    setWindowModality(Qt::WindowModal); resize(720, 520);
    connect(this, &ElaDialog::closeButtonClicked, this, &RemoteDirectoryDialog::reject);
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(20, 16, 20, 16);
    auto* note = new ElaText(tr("双击进入目录，或输入绝对路径后转到。这里只选择路径，不会自动共享。"), this);
    note->setTextPixelSize(12); note->setWordWrap(true); layout->addWidget(note);
    auto* navigation = new QHBoxLayout;
    _up = new ElaPushButton(tr("上级"), this); _up->setObjectName("remoteDirectoryUp");
    _home = new ElaPushButton(tr("主目录"), this); _home->setObjectName("remoteDirectoryHome");
    _path = new ElaLineEdit(this); _path->setObjectName("remoteDirectoryPath"); _path->setMaxLength(4096);
    _go = new ElaPushButton(tr("转到"), this); _go->setObjectName("remoteDirectoryGo");
    navigation->addWidget(_up); navigation->addWidget(_home); navigation->addWidget(_path, 1); navigation->addWidget(_go);
    layout->addLayout(navigation);
    _tree = new ElaTreeWidget(this); _tree->setObjectName("remoteDirectoryTree");
    _tree->setRootIsDecorated(false); _tree->setColumnCount(1); _tree->setHeaderLabels({tr("目录")});
    _tree->header()->setSectionResizeMode(QHeaderView::Stretch); layout->addWidget(_tree, 1);
    _status = new ElaText(this); _status->setObjectName("remoteDirectoryStatus");
    _status->setTextPixelSize(12); _status->setTextFormat(Qt::PlainText); _status->setWordWrap(true); layout->addWidget(_status);
    auto* actions = new QHBoxLayout; actions->addStretch();
    _select = new ElaPushButton(tr("选择当前目录"), this); _select->setObjectName("remoteDirectorySelect");
    auto* cancel = new ElaPushButton(tr("取消"), this); cancel->setObjectName("remoteDirectoryCancel");
    actions->addWidget(_select); actions->addWidget(cancel); layout->addLayout(actions);
    connect(_go, &QPushButton::clicked, this, [this] { load(_path->text()); });
    connect(_path, &QLineEdit::returnPressed, this, [this] { load(_path->text()); });
    connect(_path, &QLineEdit::textEdited, this, [this] { _valid = false; _select->setEnabled(false); });
    connect(_up, &QPushButton::clicked, this, [this] { load(QDir::cleanPath(_currentPath + "/..")); });
    connect(_home, &QPushButton::clicked, this, [this] { load("."); });
    const auto enterDirectory = [this](QTreeWidgetItem* item, int) {
        if (!item || _loading) return;
        const auto path = item->data(0, Qt::UserRole).toString();
        const auto revision = _revision;
        // Leave the view's mouse/key signal stack before clearing its items.
        // Some styles emit both signals; the revision admits only one request.
        QTimer::singleShot(0, this, [this, path, revision] {
            if (!_loading && revision == _revision) load(path);
        });
    };
    connect(_tree, &QTreeWidget::itemDoubleClicked, this, enterDirectory);
    connect(_tree, &QTreeWidget::itemActivated, this, enterDirectory);
    connect(_select, &QPushButton::clicked, this, &RemoteDirectoryDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &RemoteDirectoryDialog::reject);
    setBusy(false);
    QTimer::singleShot(0, this, [this, path = initialPath.isEmpty() ? QString(".") : std::move(initialPath)] { load(path); });
}

void RemoteDirectoryDialog::setBusy(bool busy)
{
    _loading = busy;
    _up->setEnabled(!busy && !_currentPath.isEmpty() && _currentPath != "/");
    _home->setEnabled(!busy); _go->setEnabled(!busy); _path->setEnabled(!busy); _tree->setEnabled(!busy);
    _select->setEnabled(!busy && _valid && shareablePath(_currentPath));
}

void RemoteDirectoryDialog::cancelRequest()
{
    ++_revision;
    delete _requestContext.data();
    _requestContext = nullptr;
}

void RemoteDirectoryDialog::load(const QString& path)
{
    // copy before setText/clear: callers may pass a QString owned by a tree item.
    const QString requested = path;
    cancelRequest(); _valid = false; _tree->clear();
    if (!_current || !_current() || !_list) {
        setBusy(false); _status->setText(tr("连接或授权已变化，请重新打开目录选择。")); return;
    }
    _path->setText(requested); _status->setText(tr("正在读取远端目录…")); setBusy(true);
    auto* context = new QObject(this); _requestContext = context;
    const QPointer<QObject> lifetime(context);
    const QPointer<RemoteDirectoryDialog> self(this);
    const auto revision = _revision;
    _list(requested, context, [self, lifetime, revision](SftpDirectoryResult result) {
        if (!self || !lifetime || revision != self->_revision) return;
        if (!result.error.isEmpty()) {
            self->_status->setText(result.error); self->setBusy(false); return;
        }
        if (!self->_current || !self->_current()) {
            self->_status->setText(tr("连接或授权已变化，请重新打开目录选择。")); self->setBusy(false); return;
        }
        if (!validPath(result.path)) {
            self->_status->setText(tr("服务器返回了无效目录路径。"));
            self->setBusy(false); return;
        }
        self->_currentPath = result.path; self->_path->setText(result.path);
        int inserted = 0;
        for (const auto& entry : std::as_const(result.entries)) {
            if ((!entry.directory && !entry.linkTargetDirectory) || entry.name == "." || entry.name == ".." || !validPath(entry.path)) continue;
            if (inserted >= 1024) { result.truncated = true; break; }
            auto* row = new QTreeWidgetItem(self->_tree, {entry.name});
            row->setData(0, Qt::UserRole, entry.path); row->setToolTip(0, entry.path); ++inserted;
        }
        self->_tree->sortItems(0, Qt::AscendingOrder);
        self->_valid = true;
        self->_status->setText(!shareablePath(result.path) && result.path != "/"
            ? tr("当前路径超过共享长度上限，可继续浏览但不能选择。")
            : result.truncated ? tr("目录条目过多，仅显示部分目录；可输入完整路径访问。")
            : result.path == "/" ? tr("根目录不能整体共享，请进入子目录。") : QString{});
        self->setBusy(false);
    });
}

void RemoteDirectoryDialog::accept()
{
    if (_loading || !_valid || !_current || !_current() || !shareablePath(_currentPath) || _path->text() != _currentPath) return;
    _selected = _currentPath; _valid = false; cancelRequest(); QDialog::accept();
}

void RemoteDirectoryDialog::reject()
{
    _valid = false; cancelRequest(); QDialog::reject();
}
