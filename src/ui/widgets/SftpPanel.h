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
class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QPaintEvent;
class QProgressBar;
class QTimer;
class QTreeWidgetItem;
class QTreeWidget;
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
    [[nodiscard]] QTreeWidgetItem* selectedItem() const;
    [[nodiscard]] QString remotePathForName(const QString& name) const;
    [[nodiscard]] bool remotePathExists(const QString& path) const;

    QLabel* _sessionLabel{nullptr};
    QLabel* _availabilityLabel{nullptr};
    QProgressBar* _uploadProgressBar{nullptr};
    QTimer* _uploadProgressDelay{nullptr};
    QLineEdit* _pathEdit{nullptr};
    ElaIconButton* _parentDirectoryButton{nullptr};
    ElaIconButton* _refreshButton{nullptr};
    ElaIconButton* _uploadButton{nullptr};
    ElaIconButton* _downloadButton{nullptr};
    QTreeWidget* _fileTree{nullptr};
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

    static constexpr int UploadProgressDelayMs = 400;
    static constexpr int UploadProgressScale = 1000;
};
