/**
 * @file   SftpSession.h
 * @brief  基于 libssh 的异步 SFTP 会话。
 */
#pragma once

#include "session/SessionTypes.h"

#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QVector>
#include <QWaitCondition>

#include <atomic>

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

    void connectToHost(const SshConfig& config);
    void disconnectFromHost();

    void listDirectory(const QString& remotePath);
    void uploadFile(const QString& localPath, const QString& remotePath);
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
    void errorOccurred(const QString& message);

private:
    enum class CommandType {
        List,
        Upload,
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
    };

    void enqueue(Command command);
    void workerMain(SshConfig config, quint64 generation);
    void postError(quint64 generation, const QString& message);
    void postDisconnected(quint64 generation);

    static constexpr int ConnectTimeoutSeconds = 10;

    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{false};
    QMutex _queueMutex;
    QWaitCondition _queueReady;
    QQueue<Command> _commands;
    QThread* _thread{nullptr};
    quint64 _generation{0};
};
