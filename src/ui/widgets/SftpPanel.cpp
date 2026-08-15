/**
 * @file SftpPanel.cpp
 * @brief SFTP 快捷传输面板布局。
 *
 * 当前工程的 SftpSession 仍是接口占位，因此控件在没有可用后端时保持禁用，
 * 明确展示原因，避免向用户伪装已经执行了文件操作。
 */
#include "SftpPanel.h"

#include "ElaIconButton.h"
#include "service/LanguageManager.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

SftpPanel::SftpPanel(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(220, 180);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    _sessionLabel = new QLabel(this);
    _sessionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(_sessionLabel);

    _availabilityLabel = new QLabel(this);
    _availabilityLabel->setWordWrap(true);
    rootLayout->addWidget(_availabilityLabel);

    // 高频文件操作集中在紧凑工具栏；按钮保留完整可访问名称和提示文本。
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

    _pathEdit = new QLineEdit(this);
    _pathEdit->setReadOnly(true);
    _pathEdit->setText(QStringLiteral("/"));
    rootLayout->addWidget(_pathEdit);

    // 文件名占据剩余空间，大小列按内容收缩，适配面板停靠到窄侧边时的布局。
    _fileTree = new QTreeWidget(this);
    _fileTree->setColumnCount(2);
    _fileTree->setRootIsDecorated(false);
    _fileTree->setUniformRowHeights(true);
    _fileTree->header()->setStretchLastSection(false);
    _fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rootLayout->addWidget(_fileTree, 1);

    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    retranslateUi();
    refreshAvailability();
}

void SftpPanel::setSessionContext(const QString& sessionLabel,
                                  bool sshSessionActive)
{
    _sessionName = sessionLabel;
    _sshSessionActive = sshSessionActive;
    refreshAvailability();
}

void SftpPanel::retranslateUi()
{
    _parentDirectoryButton->setAccessibleName(tr("Parent directory"));
    _parentDirectoryButton->setToolTip(tr("Parent directory"));
    _refreshButton->setAccessibleName(tr("Refresh"));
    _refreshButton->setToolTip(tr("Refresh"));
    _uploadButton->setAccessibleName(tr("Upload"));
    _uploadButton->setToolTip(tr("Upload"));
    _downloadButton->setAccessibleName(tr("Download"));
    _downloadButton->setToolTip(tr("Download"));
    _pathEdit->setAccessibleName(tr("Remote path"));
    _fileTree->setHeaderLabels({tr("Name"), tr("Size")});
    refreshAvailability();
}

void SftpPanel::refreshAvailability()
{
    // SftpSession 当前仍为占位实现。后端接入后只需将此门控替换为真实能力
    // 判断；在此之前必须禁用操作，避免界面产生“传输已执行”的错误反馈。
    constexpr bool backendAvailable = false;
    const bool enabled = _sshSessionActive && backendAvailable;
    _parentDirectoryButton->setEnabled(enabled);
    _refreshButton->setEnabled(enabled);
    _uploadButton->setEnabled(enabled);
    _downloadButton->setEnabled(enabled);
    _pathEdit->setEnabled(enabled);
    _fileTree->setEnabled(enabled);

    _sessionLabel->setText(_sessionName.isEmpty()
        ? tr("No active SSH session")
        : tr("Session: %1").arg(_sessionName));
    _availabilityLabel->setText(_sshSessionActive
        ? tr("SFTP transfer support is not available yet.")
        : tr("Select a connected SSH terminal to browse remote files."));

    // 用树内空状态占位，后端可用后这里将由远端目录条目替代。
    _fileTree->clear();
    auto* messageItem = new QTreeWidgetItem(
        _fileTree, {_sshSessionActive
            ? tr("SFTP backend pending")
            : tr("Waiting for an SSH session")});
    messageItem->setFlags(messageItem->flags() & ~Qt::ItemIsSelectable);
}
