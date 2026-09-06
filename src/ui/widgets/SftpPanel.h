/**
 * @file SftpPanel.h
 * @brief 可停靠的 SFTP 快捷传输面板。
 */
#pragma once

#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QWidget>

class ElaIconButton;
class ElaLineEdit;
class ElaText;
class ElaTreeWidget;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QPaintEvent;
class QProgressBar;
class QTimer;
class QTreeWidgetItem;
class SftpSession;
class SshTransport;

class SftpPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SftpPanel(QWidget* parent = nullptr);

    /** 绑定当前已连接 SSH 终端；其他终端传入 nullptr 并保持禁用占位界面。 */
    void setSessionContext(const QString& sessionLabel,
                           SshTransport* transport);
    /** 使用当前 SSH Shell 上报的目录刷新 SFTP 面板。 */
    void synchronizePathFromTerminal(const QString& path);
    /** 结束反向同步等待并显示简短错误。 */
    void terminalPathLookupFailed();

signals:
    /** 请求把当前远端目录粘贴到当前 SSH 终端，不自动执行。 */
    void pastePathToTerminalRequested(const QString& path);
    /** 请求把当前 SSH 终端的工作目录同步为当前远端目录。 */
    void synchronizeTerminalPathRequested(const QString& path);
    /** 请求读取当前 SSH Shell 目录，以反向同步 SFTP 面板。 */
    void synchronizeSftpPathFromTerminalRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct UploadRequest
    {
        QString localPath;
        QString remotePath;
        quint64 size{0};
        bool directory{false};
    };

    void retranslateUi();
    /**
     * @brief 显示或清除文件列表的空状态提示。
     * @param text 提示文案；传空字符串则隐藏提示。
     * @note  提示用 ElaText 而不是往树里塞一条占位 QTreeWidgetItem —— 无会话时
     *        整棵树是禁用的，Ela 的树样式会把 item 文字画成 BasicTextDisable
     *        （`ElaTreeViewStyle.cpp:226`），占位文案因此显示为几乎看不见的浅灰。
     */
    void setFileTreeHint(const QString& text);
    void refreshAvailability();
    void setBusy(bool busy, const QString& message = {});
    void setDropActive(bool active);
    void requestDirectory(const QString& path);
    void uploadFile();
    void queueUploads(const QStringList& localPaths);
    void startNextUpload();
    void finishUploadBatch();
    void startUploadProgress(quint64 totalBytes);
    void updateUploadProgress(quint64 transferred, quint64 totalBytes);
    void stopUploadProgress();
    void downloadSelectedFile();
    void showFileContextMenu(const QPoint& position);
    void updateSelectionActions();
    void updateFileTreeIcons();
    void sortFileTree(Qt::SortOrder order);
    [[nodiscard]] QTreeWidgetItem* selectedItem() const;
    [[nodiscard]] QString remotePathForName(const QString& name) const;
    [[nodiscard]] bool remotePathExists(const QString& path) const;

    ElaText* _availabilityLabel{nullptr};
    QProgressBar* _uploadProgressBar{nullptr};
    QTimer* _uploadProgressDelay{nullptr};
    ElaLineEdit* _pathEdit{nullptr};
    ElaIconButton* _parentDirectoryButton{nullptr};
    ElaIconButton* _refreshButton{nullptr};
    ElaIconButton* _pastePathButton{nullptr};
    ElaIconButton* _synchronizePathButton{nullptr};
    ElaIconButton* _synchronizeFromTerminalPathButton{nullptr};
    ElaIconButton* _uploadButton{nullptr};
    ElaIconButton* _downloadButton{nullptr};
    ElaTreeWidget* _fileTree{nullptr};
    ElaText* _fileTreeHint{nullptr};
    SftpSession* _sftpSession{nullptr};
    QPointer<SshTransport> _sshTransport;
    QQueue<UploadRequest> _pendingUploads;
    QString _sessionName;
    QString _currentPath{QStringLiteral("/")};
    QString _activeUploadLocalPath;
    QString _activeUploadRemotePath;
    QString _lastUploadLog;
    QString _lastUploadError;
    QString _lastUploadErrorDetail;
    QString _statusBeforeDrop;
    QString _statusAfterNextDirectoryList;
    int _uploadTotal{0};
    int _uploadCompleted{0};
    int _uploadFailed{0};
    int _uploadSkipped{0};
    quint64 _activeUploadSize{0};
    bool _backendConnected{false};
    bool _busy{false};
    bool _hasError{false};
    bool _uploadBatchActive{false};
    bool _dropActive{false};
    Qt::SortOrder _nameSortOrder{Qt::AscendingOrder};

    static constexpr int UploadProgressDelayMs = 400;
    static constexpr int UploadProgressScale = 1000;
};
