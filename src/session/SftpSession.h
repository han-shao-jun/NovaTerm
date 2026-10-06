/**
 * @file   SftpSession.h
 * @brief  基于 libssh 的异步 SFTP 会话。
 */
#pragma once

#include "session/SessionTypes.h"

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QVector>
#include <QWaitCondition>

#include <atomic>
#include <memory>

class QThread;

/** 远端目录中的一个条目。 */
struct SftpFileInfo
{
    QString name;
    QString path;
    QString linkTarget;
    quint64 size{0};
    qint64 modifiedSeconds{0};
    quint32 permissions{0};
    bool directory{false};
    bool symbolicLink{false};
    bool hardLink{false};
    bool linkTargetDirectory{false};
    bool brokenSymbolicLink{false};
};

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
    void uploadFile(const QString& localPath, const QString& remotePath);
    /** @brief 将有界内存内容上传到远端文件；成功入队返回 true。 */
    [[nodiscard]] bool uploadBytes(quint64 requestId, QByteArray content,
                                   const QString& remotePath);
    /** @brief 取消指定的内存上传，活动写入会在下一个分块边界停止。 */
    void cancelUpload(quint64 requestId);
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

signals:
    void connected(const QString& homePath);
    void disconnected();
    void directoryListed(const QString& path,
                         const QVector<SftpFileInfo>& entries);
    void transferProgress(const QString& remotePath,
                          quint64 transferred, quint64 total);
    void operationFinished(const QString& operation,
                           const QString& remotePath);
    void uploadBytesFinished(quint64 requestId, const QString& remotePath,
                             bool success, const QString& errorCode);
    void errorOccurred(const QString& message);

private:
    enum class CommandType {
        List,
        Upload,
        UploadBytes,
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
    };

    /**
     * @brief 跨线程的停止标志与世代号，**故意**放在对象之外。
     *
     * SFTP worker 用的是阻塞式 libssh 调用：一旦卡在 sftp_read/sftp_write
     * 内部就看不到停止请求，而 GUI 侧不可能无限等它（详见 disconnectFromHost
     * 里的上界与放弃策略）。放弃时线程会自毁继续跑一小段，因此停止标志必须
     * 比 SftpSession 活得久 —— 放在对象里的话，僵尸线程读到的就是已释放内存。
     * 该控制块由创建线程的 lambda 按值捕获。
     */
    struct WorkerControl {
        std::atomic<bool> running{false};
        std::atomic<quint64> generation{0};
    };
    void enqueue(Command command);
    void workerMain(std::shared_ptr<WorkerControl> control, SshConfig config,
                    QString expectedHostKeyFingerprint,
                    QString trustedKnownHostsPath, quint64 generation);
    void postError(quint64 generation, const QString& message);
    void postDisconnected(quint64 generation);
    [[nodiscard]] bool isUploadCancelled(quint64 requestId);

    static constexpr int ConnectTimeoutSeconds = 10;
    // 停止请求后等待 worker 退出的上界。取 15s 与 SshTransport::TeardownWaitMs
    // 一致：ConnectTimeoutSeconds 只约束连接/认证阶段，数据阶段是无限阻塞。
    static constexpr int TeardownWaitMs = 15000;
    // 析构路径更有耐心，等满阻塞调用串行的最坏场景。
    static constexpr int TeardownDestructorWaitMs = 60000;
    static constexpr qsizetype MaxQueuedCommands = 256;
    static constexpr qsizetype MaxQueuedUploadBytes = 2 * 1024 * 1024;

    std::shared_ptr<WorkerControl> _control;
    std::atomic<bool> _connected{false};
    QMutex _queueMutex;
    QWaitCondition _queueReady;
    QQueue<Command> _commands;
    QSet<quint64> _cancelledUploadRequests;
    qsizetype _queuedUploadBytes{0};
    quint64 _activeUploadRequestId{0};
    QThread* _thread{nullptr};
    // true 表示上一任 worker 超过上界仍未退出、已被放弃（自毁），它可能仍在
    // 运行；此时禁止再启动新会话。仅 GUI 线程读写。
    bool _workerAbandoned{false};
};
