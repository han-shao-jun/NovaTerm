/**
 * @file SftpPanel.cpp
 * @brief SFTP 文件浏览与传输面板。
 */
#include "SftpPanel.h"

#include "ElaCheckBox.h"
#include "ElaDialog.h"
#include "ElaIcon.h"
#include "ElaIconButton.h"
#include "ElaLineEdit.h"
#include "ElaMenu.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "service/LanguageManager.h"
#include "session/SftpSession.h"
#include "transport/SshTransport.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QProgressBar>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyleOptionMenuItem>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>

namespace {

enum ItemDataRole {
    RemotePathRole = Qt::UserRole,
    DirectoryRole,
    SymbolicLinkRole,
    HardLinkRole,
    PermissionsRole
};

/** 在 Ela 菜单原有绘制之上，仅将危险操作重绘为主题危险色。 */
class SftpContextMenu final : public ElaMenu
{
public:
    explicit SftpContextMenu(QWidget* parent = nullptr)
        : ElaMenu(parent)
    {
    }

    void setDangerAction(QAction* action) noexcept
    {
        _dangerAction = action;
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        ElaMenu::paintEvent(event);
        if (!_dangerAction || !_dangerAction->isVisible())
            return;

        QStyleOptionMenuItem option;
        initStyleOption(&option, _dangerAction);
        option.rect = actionGeometry(_dangerAction);
        const QColor dangerColor = ElaThemeColor(
            eTheme->getThemeMode(), StatusDanger);
        option.palette.setColor(QPalette::Text, dangerColor);
        option.palette.setColor(QPalette::ButtonText, dangerColor);
        option.palette.setColor(QPalette::HighlightedText, dangerColor);

        QPainter painter(this);
        style()->drawControl(QStyle::CE_MenuItem, &option, &painter, this);
    }

private:
    QAction* _dangerAction{nullptr};
};

/** 文件权限编辑对话框：复选框与八进制权限值保持双向同步。 */
class SftpPermissionsDialog final : public ElaDialog
{
public:
    SftpPermissionsDialog(const QString& fileName, quint32 permissions,
                          QWidget* parent = nullptr)
        : ElaDialog(parent)
        , _permissions(permissions & 07777u)
    {
        setWindowTitle(SftpPanel::tr("Permissions"));
        setWindowModality(Qt::ApplicationModal);
        setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
        setAppBarHeight(30);
        setIsFixedSize(true);
        // 收紧垂直空间，避免权限项下方出现过多空白。
        setFixedSize(560, 400);

        auto* rootLayout = new QVBoxLayout(this);
        // 内容标题紧贴自定义标题栏下方，释放更多垂直空间给权限选项。
        rootLayout->setContentsMargins(32, 16, 32, 24);
        rootLayout->setSpacing(10);

        auto* title = new ElaText(
            SftpPanel::tr("Modify file permissions"), this);
        title->setTextStyle(ElaTextType::Title);
        // 权限对话框空间紧凑，缩小标题字号以优先展示下方操作项。
        title->setTextPixelSize(24);
        rootLayout->addWidget(title);

        auto* fileNameLabel = new ElaText(fileName, this);
        fileNameLabel->setTextStyle(ElaTextType::Body);
        fileNameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rootLayout->addWidget(fileNameLabel);
        rootLayout->addSpacing(4);

        auto* permissionsLayout = new QGridLayout;
        permissionsLayout->setHorizontalSpacing(24);
        permissionsLayout->setVerticalSpacing(4);
        addPermissionGroup(permissionsLayout, 0,
                           SftpPanel::tr("Owner"), 0);
        addPermissionGroup(permissionsLayout, 2,
                           SftpPanel::tr("Group"), 3);
        addPermissionGroup(permissionsLayout, 4,
                           SftpPanel::tr("Others"), 6);
        rootLayout->addLayout(permissionsLayout);
        rootLayout->addSpacing(8);

        auto* octalLayout = new QHBoxLayout;
        octalLayout->setSpacing(16);
        auto* octalLabel = new ElaText(
            SftpPanel::tr("Octal (&O)"), this);
        octalLabel->setTextStyle(ElaTextType::Body);
        _octalEdit = new ElaLineEdit(this);
        _octalEdit->setMaxLength(4);
        _octalEdit->setAlignment(Qt::AlignLeft);
        _octalEdit->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[0-7]{1,4}")),
            _octalEdit));
        octalLabel->setBuddy(_octalEdit);
        octalLayout->addWidget(octalLabel);
        octalLayout->addWidget(_octalEdit, 1);
        rootLayout->addLayout(octalLayout);
        rootLayout->addStretch();

        auto* buttonLayout = new QHBoxLayout;
        buttonLayout->setSpacing(12);
        buttonLayout->addStretch();
        auto* cancelButton = new ElaPushButton(
            SftpPanel::tr("Cancel"), this);
        cancelButton->setMinimumSize(88, 40);
        _confirmButton = new ElaPushButton(
            SftpPanel::tr("Confirm"), this);
        _confirmButton->setMinimumSize(88, 40);
        // 确认按钮使用主题主色，与项目其他确认对话框保持一致。
        _confirmButton->setLightDefaultColor(
            ElaThemeColor(ElaThemeType::Light, PrimaryNormal));
        _confirmButton->setLightHoverColor(
            ElaThemeColor(ElaThemeType::Light, PrimaryHover));
        _confirmButton->setLightPressColor(
            ElaThemeColor(ElaThemeType::Light, PrimaryPress));
        _confirmButton->setLightTextColor(Qt::white);
        _confirmButton->setDarkDefaultColor(
            ElaThemeColor(ElaThemeType::Dark, PrimaryNormal));
        _confirmButton->setDarkHoverColor(
            ElaThemeColor(ElaThemeType::Dark, PrimaryHover));
        _confirmButton->setDarkPressColor(
            ElaThemeColor(ElaThemeType::Dark, PrimaryPress));
        _confirmButton->setDarkTextColor(Qt::black);
        buttonLayout->addWidget(cancelButton);
        buttonLayout->addWidget(_confirmButton);
        rootLayout->addLayout(buttonLayout);

        connect(cancelButton, &QPushButton::clicked,
                this, &QDialog::reject);
        connect(_confirmButton, &QPushButton::clicked,
                this, &QDialog::accept);
        connect(_octalEdit, &QLineEdit::textEdited,
                this, [this](const QString& text) {
            bool valid = false;
            const uint value = text.toUInt(&valid, 8);
            _confirmButton->setEnabled(valid && value <= 07777u);
            if (!valid || value > 07777u)
                return;
            _permissions = value;
            updateChecksFromPermissions();
        });

        updateChecksFromPermissions();
        updateOctalFromChecks();
        _octalEdit->setFocus();
        _octalEdit->selectAll();
    }

    [[nodiscard]] quint32 permissions() const noexcept
    {
        return _permissions;
    }

private:
    void addPermissionGroup(QGridLayout* layout, int row,
                            const QString& groupName, int firstCheckBox)
    {
        auto* groupLabel = new ElaText(groupName, this);
        groupLabel->setTextStyle(ElaTextType::Body);
        layout->addWidget(groupLabel, row, 0, 1, 3);

        static const std::array<QString, 3> labels{
            SftpPanel::tr("Read"),
            SftpPanel::tr("Write"),
            SftpPanel::tr("Execute")};
        for (int column = 0; column < 3; ++column) {
            auto* checkBox = new ElaCheckBox(labels[column], this);
            _checkBoxes[static_cast<size_t>(firstCheckBox + column)] =
                checkBox;
            layout->addWidget(checkBox, row + 1, column);
            connect(checkBox, &QCheckBox::toggled,
                    this, [this](bool) { updateOctalFromChecks(); });
        }
    }

    void updateChecksFromPermissions()
    {
        static constexpr std::array<quint32, 9> permissionBits{
            0400u, 0200u, 0100u,
            0040u, 0020u, 0010u,
            0004u, 0002u, 0001u};
        for (size_t index = 0; index < _checkBoxes.size(); ++index) {
            const QSignalBlocker blocker(_checkBoxes[index]);
            _checkBoxes[index]->setChecked(
                (_permissions & permissionBits[index]) != 0);
        }
    }

    void updateOctalFromChecks()
    {
        static constexpr std::array<quint32, 9> permissionBits{
            0400u, 0200u, 0100u,
            0040u, 0020u, 0010u,
            0004u, 0002u, 0001u};
        quint32 basicPermissions = 0;
        for (size_t index = 0; index < _checkBoxes.size(); ++index) {
            if (_checkBoxes[index] && _checkBoxes[index]->isChecked())
                basicPermissions |= permissionBits[index];
        }
        // 九个复选框仅修改 rwx 位，保留八进制栏中设置的特殊权限位。
        _permissions = (_permissions & 07000u) | basicPermissions;
        if (_octalEdit) {
            const QSignalBlocker blocker(_octalEdit);
            _octalEdit->setText(QStringLiteral("%1").arg(
                _permissions, 4, 8, QLatin1Char('0')));
        }
        if (_confirmButton)
            _confirmButton->setEnabled(true);
    }

    std::array<ElaCheckBox*, 9> _checkBoxes{};
    ElaLineEdit* _octalEdit{nullptr};
    ElaPushButton* _confirmButton{nullptr};
    quint32 _permissions{0};
};

QString parentRemotePath(const QString& path)
{
    const QString cleaned = QDir::cleanPath(path);
    if (cleaned == QStringLiteral("/") || cleaned == QStringLiteral("."))
        return cleaned;
    const qsizetype slash = cleaned.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : cleaned.left(slash);
}

bool isValidRemoteName(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".")
        && name != QStringLiteral("..")
        && !name.contains(QLatin1Char('/'));
}

QString compactUploadError(const QString& message)
{
    // 单行区域优先完整显示常见短错误，异常长的服务端文本再截断。
    constexpr qsizetype MaxCharacters = 100;
    const QString compact = message.simplified();
    return compact.size() <= MaxCharacters
        ? compact
        : compact.left(MaxCharacters - 1) + QChar(0x2026);
}

QStringList localEntriesFromMimeData(const QMimeData* mimeData)
{
    QStringList entries;
    if (!mimeData || !mimeData->hasUrls())
        return entries;
    for (const QUrl& url : mimeData->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        const QFileInfo info(path);
        if (info.exists() && !info.isSymLink()
            && (info.isFile() || info.isDir())) {
            entries.push_back(path);
        }
    }
    return entries;
}

} // namespace

SftpPanel::SftpPanel(QWidget* parent)
    : QWidget(parent)
    , _sftpSession(new SftpSession(this))
{
    setMinimumSize(220, 180);
    setAcceptDrops(true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    _sessionLabel = new QLabel(this);
    _sessionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(_sessionLabel);

    _availabilityLabel = new QLabel(this);
    _availabilityLabel->setWordWrap(true);
    _availabilityLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(_availabilityLabel);

    // 进度条延迟显示：快速上传在 400ms 内完成时始终保持隐藏，避免界面闪烁。
    _uploadProgressBar = new QProgressBar(this);
    _uploadProgressBar->setRange(0, UploadProgressScale);
    _uploadProgressBar->setValue(0);
    _uploadProgressBar->setTextVisible(true);
    _uploadProgressBar->hide();
    rootLayout->addWidget(_uploadProgressBar);

    _uploadProgressDelay = new QTimer(this);
    _uploadProgressDelay->setSingleShot(true);
    _uploadProgressDelay->setInterval(UploadProgressDelayMs);
    connect(_uploadProgressDelay, &QTimer::timeout, this, [this]() {
        if (_uploadBatchActive && !_activeUploadRemotePath.isEmpty())
            _uploadProgressBar->show();
    });

    // 高频操作保留在紧凑工具栏，低频文件管理操作放到右键菜单。
    auto* toolbarLayout = new QHBoxLayout;
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);

    _parentDirectoryButton = new ElaIconButton(
        ElaIconType::ArrowUp, 13, 30, 30, this);
    _refreshButton = new ElaIconButton(
        ElaIconType::ArrowRotateRight, 13, 30, 30, this);
    _uploadButton = new ElaIconButton(
        ElaIconType::Upload, 13, 30, 30, this);
    _downloadButton = new ElaIconButton(
        ElaIconType::Download, 13, 30, 30, this);
    toolbarLayout->addWidget(_parentDirectoryButton);
    toolbarLayout->addWidget(_refreshButton);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(_uploadButton);
    toolbarLayout->addWidget(_downloadButton);
    rootLayout->addLayout(toolbarLayout);

    // SSH 激活时路径框可直接输入绝对路径；非 SSH 状态仍保持原有禁用外观。
    _pathEdit = new QLineEdit(this);
    _pathEdit->setText(QStringLiteral("/"));
    rootLayout->addWidget(_pathEdit);

    _fileTree = new QTreeWidget(this);
    _fileTree->setColumnCount(2);
    _fileTree->setRootIsDecorated(false);
    _fileTree->setUniformRowHeights(true);
    _fileTree->setIconSize(QSize(18, 18));
    _fileTree->setContextMenuPolicy(Qt::CustomContextMenu);
    _fileTree->header()->setStretchLastSection(false);
    _fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rootLayout->addWidget(_fileTree, 1);

    connect(_parentDirectoryButton, &QAbstractButton::clicked, this, [this]() {
        requestDirectory(parentRemotePath(_currentPath));
    });
    connect(_refreshButton, &QAbstractButton::clicked, this, [this]() {
        requestDirectory(_currentPath);
    });
    connect(_uploadButton, &QAbstractButton::clicked,
            this, &SftpPanel::uploadFile);
    connect(_downloadButton, &QAbstractButton::clicked,
            this, &SftpPanel::downloadSelectedFile);
    connect(_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        const QString path = _pathEdit->text().trimmed();
        if (!path.isEmpty())
            requestDirectory(path);
    });
    connect(_fileTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
        if (!item || _busy)
            return;
        if (item->data(0, DirectoryRole).toBool())
            requestDirectory(item->data(0, RemotePathRole).toString());
        else
            downloadSelectedFile();
    });
    connect(_fileTree, &QTreeWidget::itemSelectionChanged,
            this, &SftpPanel::updateSelectionActions);
    connect(_fileTree, &QWidget::customContextMenuRequested,
            this, &SftpPanel::showFileContextMenu);

    // 后端信号已经通过 QueuedConnection 回到 GUI 线程，可直接更新控件。
    connect(_sftpSession, &SftpSession::connected, this,
            [this](const QString& homePath) {
        _backendConnected = true;
        _hasError = false;
        requestDirectory(homePath);
    });
    connect(_sftpSession, &SftpSession::directoryListed, this,
            [this](const QString& path, QVector<SftpFileInfo> entries) {
        QCollator collator;
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        collator.setNumericMode(true);
        std::sort(entries.begin(), entries.end(),
                  [&collator](const SftpFileInfo& left,
                              const SftpFileInfo& right) {
            if (left.directory != right.directory)
                return left.directory;
            return collator.compare(left.name, right.name) < 0;
        });

        _fileTree->clear();
        for (const SftpFileInfo& entry : entries) {
            const QString size = entry.directory
                ? QStringLiteral("—")
                : QLocale().formattedDataSize(
                      static_cast<qint64>(entry.size));
            auto* item = new QTreeWidgetItem(_fileTree, {entry.name, size});
            item->setData(0, RemotePathRole, entry.path);
            item->setData(0, DirectoryRole, entry.directory);
            item->setData(0, SymbolicLinkRole, entry.symbolicLink);
            item->setData(0, HardLinkRole, entry.hardLink);
            item->setData(0, PermissionsRole, entry.permissions);
            QStringList details;
            if (entry.symbolicLink)
                details.push_back(tr("Symbolic link"));
            else if (entry.hardLink)
                details.push_back(tr("Hard link"));
            if (entry.modifiedSeconds > 0) {
                details.push_back(tr("Modified: %1").arg(
                    QLocale().toString(QDateTime::fromSecsSinceEpoch(
                        entry.modifiedSeconds), QLocale::ShortFormat)));
            }
            item->setToolTip(0, details.join(QLatin1Char('\n')));
        }
        updateFileTreeIcons();
        _currentPath = path;
        _pathEdit->setText(path);
        const bool uploadBatchRefresh =
            !_statusAfterNextDirectoryList.isEmpty();
        const QString status = !uploadBatchRefresh
            ? tr("%1 items").arg(entries.size())
            : _statusAfterNextDirectoryList;
        _statusAfterNextDirectoryList.clear();
        if (!uploadBatchRefresh || _uploadFailed == 0)
            _hasError = false;
        setBusy(false, status);
        if (uploadBatchRefresh) {
            _uploadTotal = 0;
            _uploadCompleted = 0;
            _uploadFailed = 0;
            _uploadSkipped = 0;
            _lastUploadLog.clear();
            _lastUploadError.clear();
            _lastUploadErrorDetail.clear();
        }
    });
    connect(_sftpSession, &SftpSession::transferProgress, this,
            [this](const QString&, quint64 transferred, quint64 total) {
        const int percent = total > 0
            ? static_cast<int>((transferred * 100) / total) : 0;
        if (_uploadBatchActive) {
            updateUploadProgress(transferred, total);
            return;
        }
        _availabilityLabel->setText(total > 0
            ? tr("Transferring… %1%").arg(percent)
            : tr("Transferring… %1").arg(
                QLocale().formattedDataSize(
                    static_cast<qint64>(transferred))));
    });
    connect(_sftpSession, &SftpSession::operationFinished, this,
            [this](const QString& operation, const QString&) {
        if (operation == QStringLiteral("upload") && _uploadBatchActive) {
            stopUploadProgress();
            _lastUploadLog = tr("Uploaded: %1")
                .arg(QFileInfo(_activeUploadLocalPath).fileName());
            _availabilityLabel->setText(_lastUploadLog);
            ++_uploadCompleted;
            _activeUploadLocalPath.clear();
            _activeUploadRemotePath.clear();
            _activeUploadSize = 0;
            startNextUpload();
            return;
        }
        const bool changesRemoteDirectory = operation != QStringLiteral("download");
        setBusy(false, operation == QStringLiteral("download")
            ? tr("Download completed.") : tr("Operation completed."));
        if (changesRemoteDirectory)
            requestDirectory(_currentPath);
    });
    connect(_sftpSession, &SftpSession::errorOccurred, this,
            [this](const QString& message) {
        _hasError = true;
        if (_uploadBatchActive && !_activeUploadRemotePath.isEmpty()) {
            stopUploadProgress();
            ++_uploadFailed;
            _lastUploadError = compactUploadError(message);
            _lastUploadErrorDetail = message.simplified();
            _lastUploadLog = tr("Upload failed: %1 — %2")
                .arg(QFileInfo(_activeUploadLocalPath).fileName(),
                     _lastUploadError);
            _availabilityLabel->setText(_lastUploadLog);
            _availabilityLabel->setToolTip(_lastUploadErrorDetail);
            _activeUploadLocalPath.clear();
            _activeUploadRemotePath.clear();
            _activeUploadSize = 0;
            startNextUpload();
            return;
        }
        setBusy(false, tr("Error: %1").arg(message));
    });
    connect(_sftpSession, &SftpSession::disconnected, this, [this]() {
        _backendConnected = false;
        _busy = false;
        _pendingUploads.clear();
        _activeUploadLocalPath.clear();
        _activeUploadRemotePath.clear();
        _activeUploadSize = 0;
        _uploadBatchActive = false;
        stopUploadProgress();
        setDropActive(false);
        if (!_hasError && _sshTransport)
            _availabilityLabel->setText(tr("The SFTP connection was closed."));
        refreshAvailability();
    });

    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode) { updateFileTreeIcons(); });
    retranslateUi();
    refreshAvailability();
}

void SftpPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (!_backendConnected || _busy
        || localEntriesFromMimeData(event->mimeData()).isEmpty()) {
        event->ignore();
        return;
    }

    // 文件拖入属于复制语义；用边框和文字共同反馈，不能只依赖颜色。
    event->setDropAction(Qt::CopyAction);
    event->accept();
    setDropActive(true);
}

void SftpPanel::dragMoveEvent(QDragMoveEvent* event)
{
    if (_dropActive && _backendConnected && !_busy) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    event->ignore();
}

void SftpPanel::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDropActive(false);
    event->accept();
}

void SftpPanel::dropEvent(QDropEvent* event)
{
    const QStringList localEntries = localEntriesFromMimeData(event->mimeData());
    setDropActive(false);
    if (!_backendConnected || _busy || localEntries.isEmpty()) {
        event->ignore();
        return;
    }

    event->setDropAction(Qt::CopyAction);
    event->accept();
    queueUploads(localEntries);
}

void SftpPanel::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!_dropActive)
        return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(palette().color(QPalette::Highlight), 2.0, Qt::DashLine);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect().adjusted(2, 2, -3, -3), 6, 6);
}

void SftpPanel::setSessionContext(const QString& sessionLabel,
                                  SshTransport* transport)
{
    if (transport && _sshTransport == transport
        && _sessionName == sessionLabel) {
        return;
    }

    if (_sshTransport)
        disconnect(_sshTransport, nullptr, this, nullptr);
    setDropActive(false);
    stopUploadProgress();
    _sftpSession->disconnectFromHost();
    _sshTransport = transport;
    _sessionName = sessionLabel;
    _backendConnected = false;
    _busy = false;
    _hasError = false;
    _pendingUploads.clear();
    _activeUploadLocalPath.clear();
    _activeUploadRemotePath.clear();
    _activeUploadSize = 0;
    _lastUploadLog.clear();
    _lastUploadError.clear();
    _lastUploadErrorDetail.clear();
    _statusAfterNextDirectoryList.clear();
    _uploadTotal = 0;
    _uploadCompleted = 0;
    _uploadFailed = 0;
    _uploadSkipped = 0;
    _uploadBatchActive = false;
    _currentPath = QStringLiteral("/");
    _pathEdit->setText(_currentPath);

    if (_sshTransport) {
        // 终端对象销毁或断开时立即撤销面板能力，避免继续使用失效凭据上下文。
        connect(_sshTransport, &QObject::destroyed, this, [this]() {
            setSessionContext(_sessionName, nullptr);
        });
        connect(_sshTransport, &ITransport::disconnected, this, [this]() {
            setSessionContext(_sessionName, nullptr);
        });
        _fileTree->clear();
        auto* item = new QTreeWidgetItem(
            _fileTree, {tr("Connecting to SFTP…")});
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        _availabilityLabel->setText(tr("Connecting to SFTP…"));
        refreshAvailability();
        _sftpSession->connectToHost(_sshTransport->sessionConfig());
        return;
    }

    refreshAvailability();
}

void SftpPanel::retranslateUi()
{
    setAccessibleName(tr("SFTP file transfer panel"));
    _parentDirectoryButton->setAccessibleName(tr("Parent directory"));
    _parentDirectoryButton->setToolTip(tr("Parent directory"));
    _refreshButton->setAccessibleName(tr("Refresh"));
    _refreshButton->setToolTip(tr("Refresh"));
    _uploadButton->setAccessibleName(tr("Upload"));
    _uploadButton->setToolTip(tr("Upload, or drop local files onto this panel"));
    _downloadButton->setAccessibleName(tr("Download"));
    _downloadButton->setToolTip(tr("Download"));
    _uploadProgressBar->setAccessibleName(tr("Upload progress"));
    _pathEdit->setAccessibleName(tr("Remote path"));
    _fileTree->setHeaderLabels({tr("Name"), tr("Size")});
    _fileTree->setToolTip(
        tr("Drop local files here to upload them to the current directory"));
    refreshAvailability();
}

void SftpPanel::refreshAvailability()
{
    const bool enabled = _sshTransport && _backendConnected && !_busy;
    _parentDirectoryButton->setEnabled(enabled);
    _refreshButton->setEnabled(enabled);
    _uploadButton->setEnabled(enabled);
    _pathEdit->setEnabled(enabled);
    _fileTree->setEnabled(enabled);
    _sessionLabel->setText(_sessionName.isEmpty()
        ? tr("No active SSH session")
        : tr("Session: %1").arg(_sessionName));

    if (!_sshTransport) {
        _downloadButton->setEnabled(false);
        _availabilityLabel->setText(
            tr("Select a connected SSH terminal to browse remote files."));
        _fileTree->clear();
        auto* item = new QTreeWidgetItem(
            _fileTree, {tr("Waiting for an SSH session")});
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        return;
    }

    updateSelectionActions();
}

void SftpPanel::setBusy(bool busy, const QString& message)
{
    _busy = busy;
    if (!message.isEmpty())
        _availabilityLabel->setText(message);
    refreshAvailability();
}

void SftpPanel::setDropActive(bool active)
{
    if (_dropActive == active)
        return;
    _dropActive = active;
    if (active) {
        _statusBeforeDrop = _availabilityLabel->text();
        _availabilityLabel->setText(
            tr("Release to upload files to %1").arg(_currentPath));
    } else if (!_statusBeforeDrop.isEmpty()) {
        _availabilityLabel->setText(_statusBeforeDrop);
        _statusBeforeDrop.clear();
    }
    update();
}

void SftpPanel::requestDirectory(const QString& path)
{
    if (!_backendConnected || path.trimmed().isEmpty())
        return;
    setBusy(true, tr("Loading remote directory…"));
    _sftpSession->listDirectory(path.trimmed());
}

QString SftpPanel::remotePathForName(const QString& name) const
{
    return _currentPath == QStringLiteral("/")
        ? _currentPath + name
        : _currentPath + QLatin1Char('/') + name;
}

QTreeWidgetItem* SftpPanel::selectedItem() const
{
    const QList<QTreeWidgetItem*> selected = _fileTree->selectedItems();
    return selected.isEmpty() ? nullptr : selected.constFirst();
}

void SftpPanel::updateSelectionActions()
{
    QTreeWidgetItem* item = selectedItem();
    const bool downloadable = _sshTransport && _backendConnected && !_busy
        && item && !item->data(0, SymbolicLinkRole).toBool();
    _downloadButton->setEnabled(downloadable);
}

void SftpPanel::updateFileTreeIcons()
{
    // 使用主题色生成目录和文件图标，切换明暗主题时同步刷新。
    const auto themeMode = eTheme->getThemeMode();
    const QIcon folderIcon = ElaIcon::getInstance()->getElaIcon(
        ElaIconType::FolderClosed, 16, 18, 18,
        ElaThemeColor(themeMode, PrimaryNormal));
    const QIcon fileIcon = ElaIcon::getInstance()->getElaIcon(
        ElaIconType::File, 15, 18, 18,
        ElaThemeColor(themeMode, BasicText));
    const QIcon symbolicLinkIcon = ElaIcon::getInstance()->getElaIcon(
        ElaIconType::LinkSimple, 15, 18, 18,
        ElaThemeColor(themeMode, PrimaryNormal));
    const QIcon hardLinkIcon = ElaIcon::getInstance()->getElaIcon(
        ElaIconType::LinkHorizontal, 15, 18, 18,
        ElaThemeColor(themeMode, BasicText));

    for (int index = 0; index < _fileTree->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = _fileTree->topLevelItem(index);
        if (!item->data(0, RemotePathRole).isValid())
            continue;
        if (item->data(0, SymbolicLinkRole).toBool())
            item->setIcon(0, symbolicLinkIcon);
        else if (item->data(0, HardLinkRole).toBool())
            item->setIcon(0, hardLinkIcon);
        else
            item->setIcon(0, item->data(0, DirectoryRole).toBool()
                ? folderIcon : fileIcon);
    }
}

void SftpPanel::uploadFile()
{
    if (!_backendConnected || _busy)
        return;

    SftpContextMenu menu(this);
    QAction* filesAction = menu.addAction(tr("Upload files"));
    QAction* directoryAction = menu.addAction(tr("Upload folder"));
    QAction* selectedAction = menu.exec(_uploadButton->mapToGlobal(
        QPoint(0, _uploadButton->height())));
    if (!selectedAction)
        return;

    if (selectedAction == filesAction) {
        queueUploads(QFileDialog::getOpenFileNames(
            this, tr("Upload files")));
    } else if (selectedAction == directoryAction) {
        const QString directory = QFileDialog::getExistingDirectory(
            this, tr("Upload folder"));
        if (!directory.isEmpty())
            queueUploads({directory});
    }
}

bool SftpPanel::remotePathExists(const QString& path) const
{
    for (int index = 0; index < _fileTree->topLevelItemCount(); ++index) {
        if (_fileTree->topLevelItem(index)
                ->data(0, RemotePathRole).toString() == path) {
            return true;
        }
    }
    return false;
}

void SftpPanel::queueUploads(const QStringList& localPaths)
{
    if (!_backendConnected || _busy || localPaths.isEmpty())
        return;

    QQueue<UploadRequest> requests;
    QSet<QString> remotePaths;
    int overwriteCount = 0;
    int skippedCount = 0;
    for (const QString& localPath : localPaths) {
        const QFileInfo fileInfo(localPath);
        if (!fileInfo.exists() || fileInfo.isSymLink()
            || (!fileInfo.isFile() && !fileInfo.isDir())) {
            ++skippedCount;
            continue;
        }

        const QString remotePath = remotePathForName(fileInfo.fileName());
        // 不允许同批次中两个同名本地文件互相覆盖，保留首次出现的文件。
        if (remotePaths.contains(remotePath)) {
            ++skippedCount;
            continue;
        }
        remotePaths.insert(remotePath);
        if (remotePathExists(remotePath))
            ++overwriteCount;
        requests.enqueue({fileInfo.absoluteFilePath(), remotePath,
                          fileInfo.isFile()
                              ? static_cast<quint64>(fileInfo.size()) : 0,
                          fileInfo.isDir()});
    }

    if (requests.isEmpty()) {
        _availabilityLabel->setText(
            tr("No local files or folders were available to upload."));
        return;
    }
    if (overwriteCount > 0
        && QMessageBox::question(
            this, tr("Replace remote files"),
            tr("%1 remote item(s) already exist. Merge or replace them?")
                .arg(overwriteCount),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes) {
        return;
    }

    _pendingUploads = std::move(requests);
    _uploadTotal = _pendingUploads.size();
    _uploadCompleted = 0;
    _uploadFailed = 0;
    _uploadSkipped = skippedCount;
    _lastUploadLog.clear();
    _lastUploadError.clear();
    _lastUploadErrorDetail.clear();
    _availabilityLabel->setToolTip({});
    _uploadBatchActive = true;
    setBusy(true);
    startNextUpload();
}

void SftpPanel::startNextUpload()
{
    if (!_uploadBatchActive)
        return;
    if (!_backendConnected) {
        _pendingUploads.clear();
        _activeUploadRemotePath.clear();
        _uploadBatchActive = false;
        setBusy(false, tr("Upload stopped because the SFTP connection closed."));
        return;
    }
    if (_pendingUploads.isEmpty()) {
        finishUploadBatch();
        return;
    }

    const UploadRequest request = _pendingUploads.dequeue();
    _activeUploadLocalPath = request.localPath;
    _activeUploadRemotePath = request.remotePath;
    _activeUploadSize = request.size;
    const int current = _uploadCompleted + _uploadFailed + 1;
    _lastUploadLog = _uploadTotal > 1
        ? tr("Uploading: %1 (%2/%3)")
            .arg(QFileInfo(request.localPath).fileName())
            .arg(current)
            .arg(_uploadTotal)
        : tr("Uploading: %1")
            .arg(QFileInfo(request.localPath).fileName());
    _availabilityLabel->setToolTip({});
    setBusy(true, _lastUploadLog);
    startUploadProgress(request.size);
    if (request.directory) {
        _sftpSession->uploadDirectory(
            request.localPath, request.remotePath);
    } else {
        _sftpSession->uploadFile(request.localPath, request.remotePath);
    }
}

void SftpPanel::finishUploadBatch()
{
    _uploadBatchActive = false;
    stopUploadProgress();
    _activeUploadLocalPath.clear();
    _activeUploadRemotePath.clear();
    _activeUploadSize = 0;

    if (_uploadTotal == 1 && !_lastUploadLog.isEmpty()) {
        _statusAfterNextDirectoryList = _lastUploadLog;
    } else if (_uploadFailed == 0 && _uploadSkipped == 0) {
        _statusAfterNextDirectoryList =
            tr("Uploaded %1 files").arg(_uploadCompleted);
    } else {
        _statusAfterNextDirectoryList =
            tr("Uploaded %1, failed %2, skipped %3")
                .arg(_uploadCompleted)
                .arg(_uploadFailed)
                .arg(_uploadSkipped);
        if (!_lastUploadError.isEmpty())
            _statusAfterNextDirectoryList +=
                tr(" — %1").arg(_lastUploadError);
        _availabilityLabel->setToolTip(_lastUploadErrorDetail);
    }

    // 整批完成后只刷新一次目录，避免多文件上传时重复枚举远端目录。
    if (_backendConnected)
        requestDirectory(_currentPath);
    else
        setBusy(false, _statusAfterNextDirectoryList);
}

void SftpPanel::startUploadProgress(quint64 totalBytes)
{
    stopUploadProgress();
    _activeUploadSize = totalBytes;
    _uploadProgressBar->setRange(0, UploadProgressScale);
    _uploadProgressBar->setValue(0);
    _uploadProgressBar->setFormat(QStringLiteral("%p%"));
    _uploadProgressDelay->start();
}

void SftpPanel::updateUploadProgress(quint64 transferred,
                                     quint64 totalBytes)
{
    const quint64 total = totalBytes > 0 ? totalBytes : _activeUploadSize;
    if (total == 0)
        return;

    const long double ratio = static_cast<long double>(transferred)
        / static_cast<long double>(total);
    const int value = std::clamp(
        static_cast<int>(ratio * UploadProgressScale),
        0, UploadProgressScale);
    _uploadProgressBar->setValue(value);
}

void SftpPanel::stopUploadProgress()
{
    if (_uploadProgressDelay)
        _uploadProgressDelay->stop();
    if (_uploadProgressBar) {
        _uploadProgressBar->hide();
        _uploadProgressBar->setValue(0);
    }
}

void SftpPanel::downloadSelectedFile()
{
    QTreeWidgetItem* item = selectedItem();
    if (!item || item->data(0, SymbolicLinkRole).toBool()
        || !_backendConnected || _busy) {
        return;
    }

    QString downloadDirectory =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloadDirectory.isEmpty())
        downloadDirectory = QDir::homePath();
    const bool directory = item->data(0, DirectoryRole).toBool();
    QString localPath;
    if (directory) {
        const QString parentDirectory = QFileDialog::getExistingDirectory(
            this, tr("Select download directory"), downloadDirectory);
        if (parentDirectory.isEmpty())
            return;
        localPath = QDir(parentDirectory).filePath(item->text(0));
    } else {
        localPath = QFileDialog::getSaveFileName(
            this, tr("Download file"),
            QDir(downloadDirectory).filePath(item->text(0)));
        if (localPath.isEmpty())
            return;
    }

    setBusy(true, tr("Downloading %1…").arg(item->text(0)));
    const QString remotePath = item->data(0, RemotePathRole).toString();
    if (directory)
        _sftpSession->downloadDirectory(remotePath, localPath);
    else
        _sftpSession->downloadFile(remotePath, localPath);
}

void SftpPanel::showFileContextMenu(const QPoint& position)
{
    if (!_backendConnected || _busy)
        return;

    QTreeWidgetItem* item = _fileTree->itemAt(position);
    if (item)
        _fileTree->setCurrentItem(item);
    else
        _fileTree->clearSelection();

    // 文件操作在前、目录创建与刷新在后；空白处右键时保留完整菜单结构。
    SftpContextMenu menu(this);
    QAction* downloadAction = menu.addAction(tr("Download"));
    QAction* renameAction = menu.addAction(tr("Rename"));
    QAction* permissionsAction = menu.addAction(tr("Permissions"));
    QAction* copyPathAction = menu.addAction(tr("Copy path"));
    QAction* removeAction = menu.addAction(tr("Delete"));
    menu.setDangerAction(removeAction);
    menu.addSeparator();
    QAction* createDirectoryAction = menu.addAction(tr("New folder"));
    QAction* createFileAction = menu.addAction(tr("New file"));
    menu.addSeparator();
    QAction* refreshAction = menu.addAction(tr("Refresh"));

    const bool hasItem = item != nullptr;
    const bool symbolicLink = hasItem
        && item->data(0, SymbolicLinkRole).toBool();
    downloadAction->setEnabled(
        hasItem && !symbolicLink);
    renameAction->setEnabled(hasItem);
    permissionsAction->setEnabled(hasItem && !symbolicLink);
    copyPathAction->setEnabled(hasItem);
    removeAction->setEnabled(hasItem);

    QAction* selectedAction = menu.exec(_fileTree->viewport()->mapToGlobal(position));
    if (!selectedAction)
        return;
    if (selectedAction == downloadAction) {
        downloadSelectedFile();
        return;
    }
    if (selectedAction == createDirectoryAction) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("New folder"), tr("Folder name:"),
            QLineEdit::Normal, {}, &accepted).trimmed();
        if (accepted && isValidRemoteName(name)) {
            setBusy(true, tr("Creating folder…"));
            _sftpSession->createDirectory(remotePathForName(name));
        }
        return;
    }
    if (selectedAction == createFileAction) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("New file"), tr("File name:"),
            QLineEdit::Normal, {}, &accepted).trimmed();
        if (accepted && isValidRemoteName(name)) {
            setBusy(true, tr("Creating file…"));
            _sftpSession->createFile(remotePathForName(name));
        }
        return;
    }
    if (selectedAction == refreshAction) {
        requestDirectory(_currentPath);
        return;
    }
    if (!item)
        return;

    const QString oldPath = item->data(0, RemotePathRole).toString();
    if (selectedAction == renameAction) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
            item->text(0), &accepted).trimmed();
        if (accepted && isValidRemoteName(name)
            && name != item->text(0)) {
            setBusy(true, tr("Renaming…"));
            _sftpSession->renameEntry(oldPath, remotePathForName(name));
        }
        return;
    }
    if (selectedAction == permissionsAction) {
        const quint32 currentPermissions =
            item->data(0, PermissionsRole).toUInt() & 07777u;
        SftpPermissionsDialog dialog(
            item->text(0), currentPermissions, this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        setBusy(true, tr("Changing permissions…"));
        _sftpSession->changePermissions(oldPath, dialog.permissions());
        return;
    }
    if (selectedAction == copyPathAction) {
        QApplication::clipboard()->setText(oldPath);
        _availabilityLabel->setText(tr("Path copied."));
        _availabilityLabel->setToolTip(oldPath);
        return;
    }
    if (selectedAction != removeAction)
        return;

    const bool directory = item->data(0, DirectoryRole).toBool();
    const QString confirmation = directory
        ? tr("Delete folder %1 and all its contents? This action cannot be undone.")
              .arg(item->text(0))
        : tr("Delete %1?").arg(item->text(0));
    if (QMessageBox::question(
            this, tr("Delete remote entry"), confirmation,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
        != QMessageBox::Yes) {
        return;
    }

    setBusy(true, tr("Deleting…"));
    _sftpSession->removeEntry(oldPath, directory);
}
