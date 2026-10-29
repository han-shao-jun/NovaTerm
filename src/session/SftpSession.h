/**
 * @file   SftpSession.h
 * @brief  基于 libssh 的异步 SFTP 会话。
 */
#pragma once

#include "session/SessionTypes.h"
#include "SftpDirectoryListing.h"
#include "session/SftpFileUpload.h"

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <functional>
#include <memory>

class QThread;
struct sftp_session_struct;
struct ssh_session_struct;
struct sftp_attributes_struct;

/**
 * SFTP 会话使用独立 SSH 连接，避免文件传输阻塞终端 shell channel。
 * 所有 libssh/SFTP 阻塞调用均在专用工作线程执行，GUI 线程只负责排队请求。
 */
class SftpSession final : public QObject
{
    Q_OBJECT

public:
    explicit SftpSession(QObject* parent = nullptr);
    ~SftpSession() override;

    SftpSession(const SftpSession&) = delete;
    SftpSession& operator=(const SftpSession&) = delete;

    void connectToHost(const SshConfig& config,
                       const QString& expectedHostKeyFingerprint = {},
                       const QString& trustedKnownHostsPath = {});
    void disconnectFromHost(bool shuttingDown = false);

    void listDirectory(const QString& remotePath);
    /** 独立只读浏览请求；0表示未受理，结果不广播给原文件面板。 */
    [[nodiscard]] quint64 requestDirectory(const QString& remotePath);
    void cancelDirectoryRequest(quint64 requestId);
    void uploadFile(const QString& localPath, const QString& remotePath);
    /** @brief 将有界内存内容上传到远端文件；成功入队返回 true。 */
    [[nodiscard]] bool uploadBytes(quint64 requestId, QByteArray content,
                                   const QString& remotePath);
    /** @brief 取消指定的内存上传，活动写入会在下一个分块边界停止。 */
    void cancelUpload(quint64 requestId);
    /** @brief 兼容入口名：按 request.direction 将受授权上传或下载排入原工作线程。 */
    [[nodiscard]] bool uploadAuthorizedFile(const SftpFileUploadRequest& request);
    void uploadDirectory(const QString& localPath, const QString& remotePath);
    void downloadFile(const QString& remotePath, const QString& localPath);
    void downloadDirectory(const QString& remotePath, const QString& localPath);
    void createDirectory(const QString& remotePath);
    void createFile(const QString& remotePath);
    void changePermissions(const QString& remotePath, quint32 permissions);
    void removeEntry(const QString& remotePath, bool directory);
    void renameEntry(const QString& oldRemotePath,
                     const QString& newRemotePath);

    [[nodiscard]] bool isConnected() const noexcept;
    /** 仅GUI线程查询；不等待旧线程，可安全发起显式重试。 */
    [[nodiscard]] bool canStartConnection() const noexcept;
    /** @brief 非阻塞读取真实队列/在途命令；取锁失败保守视为忙碌。 */
    [[nodiscard]] bool hasPendingOperations() const;

signals:
    void connected(const QString& homePath);
    void disconnected();
    void directoryListed(const QString& path,
                         const QVector<SftpFileInfo>& entries);
    void directoryRequestFinished(quint64 requestId, const SftpDirectoryResult& result);
    void transferProgress(const QString& remotePath,
                          quint64 transferred, quint64 total);
    void operationFinished(const QString& operation,
                           const QString& remotePath);
    void uploadBytesFinished(quint64 requestId, const QString& remotePath,
                             bool success, const QString& errorCode);
    void authorizedFileProgress(quint64 requestId, quint64 transferred, quint64 total);
    void authorizedFilePublicationReady(quint64 requestId);
    void authorizedFileFinished(const SftpFileUploadResult& result);
    void errorOccurred(const QString& message);

private:
    friend class SftpSessionSafetyTestAccess;
    enum class CommandType {
        List,
        Upload,
        UploadBytes,
        UploadAuthorizedFile,
        UploadDirectory,
        Download,
        DownloadDirectory,
        MakeRemoteDirectory,
        CreateRemoteFile,
        ChangePermissions,
        RemoveFile,
        DeleteRemoteDirectory,
        Rename
    };

    struct Command
    {
        CommandType type{CommandType::List};
        QString source;
        QString target;
        quint32 permissions{0};
        quint64 requestId{0};
        QByteArray content{};
        SftpFileUploadRequest fileUpload{};
    };

    /** 全部工作线程状态独立于 QObject，允许有界等待后安全放弃阻塞线程。 */
    struct WorkerControl {
        WorkerControl();
        ~WorkerControl();
        std::atomic<bool> running{false};
        std::atomic<quint64> generation{0};
        std::atomic<bool> connected{false};
        QMutex queueMutex;
        QWaitCondition queueReady;
        QQueue<Command> commands;
        bool commandActive{false}; // queueMutex protects dequeue-to-completion, including blocking I/O.
        QSet<quint64> cancelledUploadRequests;
        QSet<quint64> directoryRequests;
        QSet<quint64> cancelledDirectoryRequests;
        qsizetype queuedUploadBytes{0};
        quint64 activeUploadRequestId{0};
        std::shared_ptr<SftpFileUploadControl> activeFileUpload;
        // 锁只保护入队通知时的 receiver 生命周期；绝不跨阻塞 I/O 持锁。
        QMutex notificationMutex;
        SftpSession* receiver{nullptr};
    };
    void enqueue(Command command);
    void stopWorker(int waitMs);
    static void workerMain(std::shared_ptr<WorkerControl> control,
                            SshConfig config,
                            QString expectedHostKeyFingerprint,
                            QString trustedKnownHostsPath, quint64 generation);
    static void postNotification(const std::shared_ptr<WorkerControl>& control,
                                 quint64 generation,
                                 std::function<void(SftpSession&)> callback);
    static void finishWorker(const std::shared_ptr<WorkerControl>& control,
                             quint64 generation);
    // Terminal facts survive connection generation changes; QObject context still guards lifetime.
    static void postFileUploadResult(const std::shared_ptr<WorkerControl>& control,
                                     const SftpFileUploadResult& result);
    static void postError(const std::shared_ptr<WorkerControl>& control,
                          quint64 generation, const QString& message);
    static void postDirectoryResult(const std::shared_ptr<WorkerControl>&,
        quint64 generation, quint64 requestId, SftpDirectoryResult);
    [[nodiscard]] static bool isUploadCancelled(
        const std::shared_ptr<WorkerControl>& control, quint64 requestId);
    // 每次阻塞收尾后重查取消；成功发布后保持真实提交结果。
    [[nodiscard]] static bool finalizeScriptUpload(
        const std::function<bool()>& cancelled,
        const std::function<bool()>& close,
        const std::function<bool()>& makeExecutable,
        const std::function<bool()>& publish,
        const std::function<void()>& discard, QString& error);
    [[nodiscard]] static bool uploadScriptBytes(
        sftp_session_struct* sftp, ssh_session_struct* session,
        const QString& remotePath, const QByteArray& content,
        const std::function<bool()>& cancelled, QString& error);

    [[nodiscard]] static bool authorizedUploadOwners(const QVector<quint32>& owners);
    enum class AuthorizedPublication { Rejected, CreateNew, ReplaceExisting };
    [[nodiscard]] static AuthorizedPublication authorizedUploadPublication(
        bool overwrite, const sftp_attributes_struct* target, int lookupStatus,
        quint32 scopeOwner, bool hardlinkSupported, bool renameSupported, QString& error);
    static void finalizeAuthorizedUpload(
        const std::shared_ptr<SftpFileUploadControl>& control,
        const std::function<void()>& ready,
        const std::function<bool()>& guard,
        const std::function<int()>& publish,
        const std::function<QString()>& cleanup, SftpFileUploadResult& result,
        bool consumesTemporary = false);
    static SftpFileUploadResult uploadAuthorizedFileWorker(
        sftp_session_struct* sftp, ssh_session_struct* session,
        const SftpFileUploadRequest& request,
        const std::function<bool()>& stopped,
        const std::function<void(quint64, quint64)>& progress,
        const std::function<void()>& ready);

    [[nodiscard]] static bool authorizedDownloadSource(
        const sftp_attributes_struct* attributes, quint32 scopeOwner,
        const sftp_attributes_struct* original = nullptr);
    static SftpFileUploadResult copyAuthorizedDownload(
        const SftpFileUploadRequest& request, quint64 totalBytes,
        const std::function<qint64(char*, qsizetype)>& read,
        const std::function<bool()>& close,
        const std::function<bool()>& guard, const std::function<bool()>& stopped,
        const std::function<void(quint64, quint64)>& progress,
        const std::function<void()>& ready);
    static SftpFileUploadResult downloadAuthorizedFileWorker(
        sftp_session_struct* sftp, ssh_session_struct* session,
        const SftpFileUploadRequest& request,
        const std::function<bool()>& stopped,
        const std::function<void(quint64, quint64)>& progress,
        const std::function<void()>& ready);

    static constexpr int ConnectTimeoutSeconds = 10;
    // 停止请求后等待 worker 退出的上界。取 15s 与 SshTransport::TeardownWaitMs
    // 一致：libssh 单次等待有超时，但不约束整个同步 SFTP 操作。
    static constexpr int TeardownWaitMs = 15000;
    // 析构路径更有耐心，等满阻塞调用串行的最坏场景。
    static constexpr int TeardownDestructorWaitMs = 60000;
    static constexpr qsizetype MaxQueuedCommands = 256;
    static constexpr qsizetype MaxQueuedUploadBytes = 2 * 1024 * 1024;

    std::shared_ptr<WorkerControl> _control;
    QThread* _thread{nullptr};
    // true 表示上一任 worker 超过上界仍未退出、已被放弃（自毁），它可能仍在
    // 运行；此时禁止再启动新会话。仅 GUI 线程读写。
    bool _workerAbandoned{false};
    quint64 _nextDirectoryRequestId{0};
};
