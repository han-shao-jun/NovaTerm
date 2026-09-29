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
class ElaProgressBar;
class ElaText;
class ElaTreeWidget;
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QKeyEvent;
class QPaintEvent;
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
    /**
     * @brief 拦截文件树的按键，实现 Delete 键删除当前多选。
     * @note  用事件过滤而非 `keyPressEvent`：焦点在树而非面板上，后者收不到。
     *       裸 Delete 之外（含 Ctrl/Shift/Alt 组合、以及树处于行内编辑时）一律
     *       放行，避免吞掉编辑态与将来可能要加的组合键。
     */
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct UploadRequest
    {
        QString localPath;
        QString remotePath;
        quint64 size{0};
        bool directory{false};
    };

    /** @brief 一次待删除的远端条目。 */
    struct DeleteRequest
    {
        QString remotePath;
        QString name;
        bool directory{false};
    };

    /** @brief 一次待下载的远端条目。 */
    struct DownloadRequest
    {
        QString remotePath;
        QString name;
        QString localPath;
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
    void startTransferProgress(quint64 totalBytes);
    void updateTransferProgress(quint64 transferred, quint64 totalBytes);
    void stopTransferProgress();
    /**
     * @brief 当前是否有任一方向的传输正在进行。
     * @note  进度条的延迟显示需要它：上传与下载各有一套批次状态，活动条目名
     *        非空才说明真的在传（批次收尾时标志位还没清）。
     */
    [[nodiscard]] bool isTransferActive() const;
    /**
     * @brief 下载当前选区（右键菜单与工具栏下载按钮共用）。
     * @note  多选时**只选一次目标目录**，所有条目按各自文件名落到该目录下 ——
     *        逐条弹保存对话框会让多选下载无法使用，也与文件管理器不符。
     *        选区中的软链接按既有规则跳过，并在结果里计入 skipped。
     */
    void downloadSelectedEntries();
    /** @brief 下载指定条目，供双击使用（不依赖选区状态）。 */
    void downloadEntry(QTreeWidgetItem* item);
    void queueDownloads(const QList<QTreeWidgetItem*>& items,
                        const QString& destination,
                        bool destinationIsExactFilePath);
    void startNextDownload();
    void finishDownloadBatch();
    void deleteSelectedEntries();
    void startNextDelete();
    void finishDeleteBatch();
    void showFileContextMenu(const QPoint& position);
    void updateSelectionActions();
    void updateFileTreeIcons();
    void sortFileTree(Qt::SortOrder order);
    void resetDeleteBatch();
    void resetDownloadBatch();
    [[nodiscard]] QString remotePathForName(const QString& name) const;
    [[nodiscard]] bool remotePathExists(const QString& path) const;

    ElaText* _availabilityLabel{nullptr};
    ElaProgressBar* _transferProgressBar{nullptr};
    QTimer* _transferProgressDelay{nullptr};
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
    QQueue<DeleteRequest> _pendingDeletes;
    QQueue<DownloadRequest> _pendingDownloads;
    QString _sessionName;
    QString _currentPath{QStringLiteral("/")};
    QString _activeUploadLocalPath;
    QString _activeUploadRemotePath;
    QString _activeDeleteName;
    QString _activeDownloadName;
    QString _downloadDestination;
    QString _lastUploadLog;
    QString _lastUploadError;
    QString _lastUploadErrorDetail;
    QString _statusBeforeDrop;
    QString _statusAfterNextDirectoryList;
    int _uploadTotal{0};
    int _uploadCompleted{0};
    int _uploadFailed{0};
    int _uploadSkipped{0};
    int _deleteTotal{0};
    int _deleteCompleted{0};
    int _deleteFailed{0};
    int _downloadTotal{0};
    int _downloadCompleted{0};
    int _downloadFailed{0};
    int _downloadSkipped{0};
    quint64 _activeTransferSize{0};
    bool _backendConnected{false};
    bool _busy{false};
    bool _hasError{false};
    bool _uploadBatchActive{false};
    bool _deleteBatchActive{false};
    bool _downloadBatchActive{false};
    bool _dropActive{false};
    Qt::SortOrder _nameSortOrder{Qt::AscendingOrder};

    static constexpr int TransferProgressDelayMs = 400;
    static constexpr int TransferProgressScale = 1000;
};
