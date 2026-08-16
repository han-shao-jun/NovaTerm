/**
 * @file   SftpSession.cpp
 * @brief  libssh SFTP 工作线程、目录操作与文件传输实现。
 */
#include "SftpSession.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QThread>

#include <memory>
#include <functional>
#include <utility>

#include <fcntl.h>

namespace {

using SshSessionPtr =
    std::unique_ptr<ssh_session_struct, decltype(&ssh_free)>;
using SftpSessionPtr =
    std::unique_ptr<sftp_session_struct, decltype(&sftp_free)>;
using SftpDirectoryPtr =
    std::unique_ptr<sftp_dir_struct, decltype(&sftp_closedir)>;
using SftpFilePtr =
    std::unique_ptr<sftp_file_struct, decltype(&sftp_close)>;
using SftpAttributesPtr =
    std::unique_ptr<sftp_attributes_struct, decltype(&sftp_attributes_free)>;

QString knownHostsPath()
{
    return QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
}

QString remotePathJoin(const QString& directory, const QString& name)
{
    if (directory == QStringLiteral("/"))
        return directory + name;
    return directory + QLatin1Char('/') + name;
}

QString sessionError(ssh_session session, const QString& context)
{
    return QStringLiteral("%1: %2")
        .arg(context, QString::fromUtf8(ssh_get_error(session)));
}

QString sftpError(sftp_session sftp, ssh_session session,
                  const QString& context)
{
    return QStringLiteral("%1: %2 (SFTP status %3)")
        .arg(context, QString::fromUtf8(ssh_get_error(session)))
        .arg(sftp_get_error(sftp));
}

bool authenticate(ssh_session session, const SshConfig& config,
                  QString& error)
{
    const QByteArray user = config.username.trimmed().toUtf8();
    if (config.authMethod == QStringLiteral("publickey")) {
        ssh_key privateKey = nullptr;
        const QByteArray keyPath =
            QDir::toNativeSeparators(config.privateKeyPath.trimmed()).toUtf8();
        const QByteArray passphrase = config.keyPassphrase.toUtf8();
        const int importResult = ssh_pki_import_privkey_file(
            keyPath.constData(),
            passphrase.isEmpty() ? nullptr : passphrase.constData(),
            nullptr, nullptr, &privateKey);
        if (importResult != SSH_OK || !privateKey) {
            error = sessionError(session,
                SftpSession::tr("Failed to load the SSH private key"));
            return false;
        }

        const int authResult =
            ssh_userauth_publickey(session, user.constData(), privateKey);
        ssh_key_free(privateKey);
        if (authResult != SSH_AUTH_SUCCESS) {
            error = sessionError(session,
                SftpSession::tr("SFTP public-key authentication failed"));
            return false;
        }
        return true;
    }

    const QByteArray password = config.password.toUtf8();
    if (ssh_userauth_password(session, user.constData(), password.constData())
        != SSH_AUTH_SUCCESS) {
        error = sessionError(session,
            SftpSession::tr("SFTP password authentication failed"));
        return false;
    }
    return true;
}

bool isHardLinkFromLongName(const sftp_attributes attributes)
{
    if (!attributes || attributes->type != SSH_FILEXFER_TYPE_REGULAR
        || !attributes->longname) {
        return false;
    }

    // SFTP v3 没有标准硬链接计数字段；仅接受严格的 OpenSSH ls -l 格式。
    const QStringList fields = QString::fromUtf8(attributes->longname)
        .split(QRegularExpression(QStringLiteral("\\s+")),
               Qt::SkipEmptyParts);
    if (fields.size() < 2 || fields.constFirst().size() < 10
        || !fields.constFirst().startsWith(QLatin1Char('-'))) {
        return false;
    }
    bool valid = false;
    const uint linkCount = fields.at(1).toUInt(&valid);
    return valid && linkCount > 1;
}

using TransferProgressCallback =
    std::function<void(quint64 transferred, quint64 total)>;

bool ensureRemoteDirectory(sftp_session sftp, ssh_session session,
                           const QString& remotePath, QString& error)
{
    const QByteArray encodedPath = remotePath.toUtf8();
    SftpAttributesPtr attributes{
        sftp_stat(sftp, encodedPath.constData()), &sftp_attributes_free};
    if (attributes) {
        if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
            return true;
        error = SftpSession::tr("Remote path is not a directory: %1")
                    .arg(remotePath);
        return false;
    }
    if (sftp_mkdir(sftp, encodedPath.constData(), 0755) == SSH_OK)
        return true;

    // 某些服务器在目录已存在时返回通用 FAILURE，再次 stat 可消除歧义。
    attributes.reset(sftp_stat(sftp, encodedPath.constData()));
    if (attributes && attributes->type == SSH_FILEXFER_TYPE_DIRECTORY)
        return true;
    error = sftpError(sftp, session,
        SftpSession::tr("Cannot create remote directory %1")
            .arg(remotePath));
    return false;
}

bool uploadRegularFile(sftp_session sftp, ssh_session session,
                       const QString& localPath, const QString& remotePath,
                       const std::atomic<bool>& running,
                       quint64& transferred, quint64 total,
                       quint64& lastReported,
                       const TransferProgressCallback& reportProgress,
                       QString& error)
{
    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        error = SftpSession::tr("Cannot open local file %1: %2")
                    .arg(localPath, localFile.errorString());
        return false;
    }

    const QByteArray encodedPath = remotePath.toUtf8();
    SftpFilePtr remoteFile{
        sftp_open(sftp, encodedPath.constData(),
                  O_WRONLY | O_CREAT | O_TRUNC, 0644),
        &sftp_close};
    if (!remoteFile) {
        error = sftpError(sftp, session,
            SftpSession::tr("Cannot open remote file %1").arg(remotePath));
        return false;
    }

    while (running.load(std::memory_order_acquire)) {
        const QByteArray chunk = localFile.read(64 * 1024);
        if (chunk.isEmpty()) {
            if (localFile.error() != QFileDevice::NoError) {
                error = SftpSession::tr("Failed to read local file %1: %2")
                            .arg(localPath, localFile.errorString());
                return false;
            }
            break;
        }
        qsizetype offset = 0;
        while (offset < chunk.size()
               && running.load(std::memory_order_acquire)) {
            const auto written = sftp_write(
                remoteFile.get(), chunk.constData() + offset,
                static_cast<size_t>(chunk.size() - offset));
            if (written <= 0) {
                error = sftpError(sftp, session,
                    SftpSession::tr("Failed to upload %1").arg(remotePath));
                return false;
            }
            offset += static_cast<qsizetype>(written);
            transferred += static_cast<quint64>(written);
            if (transferred - lastReported >= 256 * 1024
                || transferred == total) {
                lastReported = transferred;
                reportProgress(transferred, total);
            }
        }
    }
    if (!running.load(std::memory_order_acquire))
        return false;
    if (sftp_close(remoteFile.release()) != SSH_OK) {
        error = sftpError(sftp, session,
            SftpSession::tr("Failed to finalize remote file %1")
                .arg(remotePath));
        return false;
    }
    return true;
}

struct RemoteDownloadEntry
{
    QString remotePath;
    QString relativePath;
    quint64 size{0};
    bool directory{false};
};

bool collectRemoteDirectory(sftp_session sftp, ssh_session session,
                            const QString& rootRemotePath,
                            QVector<RemoteDownloadEntry>& entries,
                            quint64& totalBytes,
                            const std::atomic<bool>& running,
                            QString& error)
{
    QQueue<QPair<QString, QString>> pendingDirectories;
    pendingDirectories.enqueue({rootRemotePath, {}});
    while (!pendingDirectories.isEmpty()
           && running.load(std::memory_order_acquire)) {
        const auto [remoteDirectory, relativeDirectory] =
            pendingDirectories.dequeue();
        const QByteArray encodedDirectory = remoteDirectory.toUtf8();
        SftpDirectoryPtr directory{
            sftp_opendir(sftp, encodedDirectory.constData()),
            &sftp_closedir};
        if (!directory) {
            error = sftpError(sftp, session,
                SftpSession::tr("Cannot open remote directory %1")
                    .arg(remoteDirectory));
            return false;
        }

        for (;;) {
            SftpAttributesPtr attributes{
                sftp_readdir(sftp, directory.get()),
                &sftp_attributes_free};
            if (!attributes)
                break;
            const QString name = QString::fromUtf8(
                attributes->name ? attributes->name : "");
            if (name == QStringLiteral(".") || name == QStringLiteral(".."))
                continue;
            if (name.isEmpty() || name.contains(QLatin1Char('/'))
                || name.contains(QLatin1Char('\\'))) {
                error = SftpSession::tr("Unsafe remote entry name in %1")
                            .arg(remoteDirectory);
                return false;
            }

            const QString remotePath = remotePathJoin(remoteDirectory, name);
            const QString relativePath = relativeDirectory.isEmpty()
                ? name
                : relativeDirectory + QLatin1Char('/') + name;
            // 不跟随软链接，避免递归环和下载到目标目录之外。
            if (attributes->type == SSH_FILEXFER_TYPE_SYMLINK)
                continue;
            if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY) {
                entries.push_back({remotePath, relativePath, 0, true});
                pendingDirectories.enqueue({remotePath, relativePath});
                continue;
            }
            if (attributes->type != SSH_FILEXFER_TYPE_REGULAR
                && attributes->type != SSH_FILEXFER_TYPE_UNKNOWN) {
                continue;
            }
            entries.push_back(
                {remotePath, relativePath, attributes->size, false});
            totalBytes += attributes->size;
        }
        if (!sftp_dir_eof(directory.get())) {
            error = sftpError(sftp, session,
                SftpSession::tr("Failed while reading remote directory %1")
                    .arg(remoteDirectory));
            return false;
        }
    }
    return running.load(std::memory_order_acquire);
}

bool downloadRegularFile(sftp_session sftp, ssh_session session,
                         const QString& remotePath, const QString& localPath,
                         const std::atomic<bool>& running,
                         quint64& transferred, quint64 total,
                         quint64& lastReported,
                         const TransferProgressCallback& reportProgress,
                         QString& error)
{
    const QByteArray encodedPath = remotePath.toUtf8();
    SftpFilePtr remoteFile{
        sftp_open(sftp, encodedPath.constData(), O_RDONLY, 0), &sftp_close};
    if (!remoteFile) {
        error = sftpError(sftp, session,
            SftpSession::tr("Cannot open remote file %1").arg(remotePath));
        return false;
    }
    QSaveFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly)) {
        error = SftpSession::tr("Cannot create local file %1: %2")
                    .arg(localPath, localFile.errorString());
        return false;
    }

    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    while (running.load(std::memory_order_acquire)) {
        const auto bytesRead = sftp_read(
            remoteFile.get(), buffer.data(),
            static_cast<size_t>(buffer.size()));
        if (bytesRead == 0)
            break;
        if (bytesRead < 0) {
            error = sftpError(sftp, session,
                SftpSession::tr("Failed to download %1").arg(remotePath));
            return false;
        }
        if (localFile.write(buffer.constData(), bytesRead) != bytesRead) {
            error = SftpSession::tr("Failed to write local file %1: %2")
                        .arg(localPath, localFile.errorString());
            return false;
        }
        transferred += static_cast<quint64>(bytesRead);
        if (transferred - lastReported >= 256 * 1024
            || transferred == total) {
            lastReported = transferred;
            reportProgress(transferred, total);
        }
    }
    if (!running.load(std::memory_order_acquire))
        return false;
    if (!localFile.commit()) {
        error = SftpSession::tr("Failed to finalize local file %1: %2")
                    .arg(localPath, localFile.errorString());
        return false;
    }
    return true;
}

bool removeRemoteDirectoryRecursively(
    sftp_session sftp, ssh_session session, const QString& rootPath,
    const std::atomic<bool>& running, QString& error)
{
    QQueue<QString> pendingDirectories;
    QVector<QString> directories{rootPath};
    QVector<QString> entriesToUnlink;
    pendingDirectories.enqueue(rootPath);

    // 先完整枚举再开始删除，尽量在权限或读取失败时避免留下半删目录。
    while (!pendingDirectories.isEmpty()
           && running.load(std::memory_order_acquire)) {
        const QString directoryPath = pendingDirectories.dequeue();
        const QByteArray encodedDirectory = directoryPath.toUtf8();
        SftpDirectoryPtr directory{
            sftp_opendir(sftp, encodedDirectory.constData()),
            &sftp_closedir};
        if (!directory) {
            error = sftpError(sftp, session,
                SftpSession::tr("Cannot open remote directory %1")
                    .arg(directoryPath));
            return false;
        }

        for (;;) {
            SftpAttributesPtr attributes{
                sftp_readdir(sftp, directory.get()),
                &sftp_attributes_free};
            if (!attributes)
                break;
            const QString name = QString::fromUtf8(
                attributes->name ? attributes->name : "");
            if (name == QStringLiteral(".") || name == QStringLiteral(".."))
                continue;
            if (name.isEmpty() || name.contains(QLatin1Char('/'))
                || name.contains(QLatin1Char('\\'))) {
                error = SftpSession::tr("Unsafe remote entry name in %1")
                            .arg(directoryPath);
                return false;
            }

            const QString entryPath = remotePathJoin(directoryPath, name);
            if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY) {
                directories.push_back(entryPath);
                pendingDirectories.enqueue(entryPath);
            } else {
                // 软链接只删除链接自身，绝不跟随到目录外的目标。
                entriesToUnlink.push_back(entryPath);
            }
        }
        if (!sftp_dir_eof(directory.get())) {
            error = sftpError(sftp, session,
                SftpSession::tr("Failed while reading remote directory %1")
                    .arg(directoryPath));
            return false;
        }
    }
    if (!running.load(std::memory_order_acquire))
        return false;

    for (const QString& entryPath : entriesToUnlink) {
        const QByteArray encodedEntry = entryPath.toUtf8();
        if (sftp_unlink(sftp, encodedEntry.constData()) != SSH_OK) {
            error = sftpError(sftp, session,
                SftpSession::tr("Cannot delete remote entry %1")
                    .arg(entryPath));
            return false;
        }
    }
    // 子目录必须从最深层向根目录逆序删除。
    for (qsizetype index = directories.size(); index > 0; --index) {
        const QString& directoryPath = directories.at(index - 1);
        const QByteArray encodedDirectory = directoryPath.toUtf8();
        if (sftp_rmdir(sftp, encodedDirectory.constData()) != SSH_OK) {
            error = sftpError(sftp, session,
                SftpSession::tr("Cannot delete remote directory %1")
                    .arg(directoryPath));
            return false;
        }
    }
    return true;
}

} // namespace

SftpSession::SftpSession(QObject* parent)
    : QObject(parent)
{
    // 静态链接 libssh 时需显式初始化；libssh 内部引用计数允许多个会话配对调用。
    ssh_init();
}

SftpSession::~SftpSession()
{
    disconnectFromHost();
    ssh_finalize();
}

void SftpSession::connectToHost(const SshConfig& config)
{
    disconnectFromHost();
    if (!config.isValid()) {
        emit errorOccurred(tr("Invalid SSH configuration for SFTP."));
        return;
    }

    // SSH 终端连接成功后 known_hosts 目录通常已存在，此处仍保证独立使用安全。
    QDir().mkpath(QFileInfo(knownHostsPath()).absolutePath());
    const quint64 generation = ++_generation;
    _running.store(true, std::memory_order_release);
    _connected.store(false, std::memory_order_release);
    _thread = QThread::create(
        [this, config, generation]() mutable {
            workerMain(std::move(config), generation);
        });
    _thread->setObjectName(QStringLiteral("SftpSession"));
    _thread->start();
}

void SftpSession::disconnectFromHost()
{
    ++_generation; // 使已排队的旧会话 GUI 回调失效。
    _running.store(false, std::memory_order_release);
    _connected.store(false, std::memory_order_release);
    {
        QMutexLocker lock(&_queueMutex);
        _commands.clear();
        _queueReady.wakeAll();
    }

    if (_thread) {
        // libssh 阻塞调用受连接超时限制；析构前必须等待线程退出，避免悬垂访问。
        _thread->wait();
        delete _thread;
        _thread = nullptr;
    }
}

bool SftpSession::isConnected() const noexcept
{
    return _connected.load(std::memory_order_acquire);
}

void SftpSession::enqueue(Command command)
{
    if (!_running.load(std::memory_order_acquire))
        return;
    QMutexLocker lock(&_queueMutex);
    _commands.enqueue(std::move(command));
    _queueReady.wakeOne();
}

void SftpSession::listDirectory(const QString& remotePath)
{
    enqueue({CommandType::List, remotePath, {}});
}

void SftpSession::uploadFile(const QString& localPath,
                             const QString& remotePath)
{
    enqueue({CommandType::Upload, localPath, remotePath});
}

void SftpSession::uploadDirectory(const QString& localPath,
                                  const QString& remotePath)
{
    enqueue({CommandType::UploadDirectory, localPath, remotePath});
}

void SftpSession::downloadFile(const QString& remotePath,
                               const QString& localPath)
{
    enqueue({CommandType::Download, remotePath, localPath});
}

void SftpSession::downloadDirectory(const QString& remotePath,
                                    const QString& localPath)
{
    enqueue({CommandType::DownloadDirectory, remotePath, localPath});
}

void SftpSession::createDirectory(const QString& remotePath)
{
    enqueue({CommandType::MakeRemoteDirectory, remotePath, {}});
}

void SftpSession::createFile(const QString& remotePath)
{
    enqueue({CommandType::CreateRemoteFile, remotePath, {}});
}

void SftpSession::changePermissions(const QString& remotePath,
                                    quint32 permissions)
{
    enqueue({CommandType::ChangePermissions, remotePath, {}, permissions});
}

void SftpSession::removeEntry(const QString& remotePath, bool directory)
{
    enqueue({directory ? CommandType::DeleteRemoteDirectory
                       : CommandType::RemoveFile,
             remotePath, {}});
}

void SftpSession::renameEntry(const QString& oldRemotePath,
                              const QString& newRemotePath)
{
    enqueue({CommandType::Rename, oldRemotePath, newRemotePath});
}

void SftpSession::postError(quint64 generation, const QString& message)
{
    QMetaObject::invokeMethod(this, [this, generation, message]() {
        if (_generation == generation)
            emit errorOccurred(message);
    }, Qt::QueuedConnection);
}

void SftpSession::postDisconnected(quint64 generation)
{
    QMetaObject::invokeMethod(this, [this, generation]() {
        if (_generation == generation)
            emit disconnected();
    }, Qt::QueuedConnection);
}

void SftpSession::workerMain(SshConfig config, quint64 generation)
{
    // 无论连接在哪个阶段退出，都统一清理原子状态并通知 GUI，避免失败路径漏状态。
    const auto workerCleanup = qScopeGuard([this, generation]() {
        _connected.store(false, std::memory_order_release);
        _running.store(false, std::memory_order_release);
        postDisconnected(generation);
    });

    SshSessionPtr session{ssh_new(), &ssh_free};
    if (!session) {
        postError(generation, tr("Failed to create the SFTP SSH session."));
        return;
    }

    const QByteArray host = config.host.trimmed().toUtf8();
    const QByteArray user = config.username.trimmed().toUtf8();
    const QByteArray knownHosts =
        QDir::toNativeSeparators(knownHostsPath()).toUtf8();
    int port = static_cast<int>(config.port);
    long timeout = ConnectTimeoutSeconds;
    const char* hostKeyAlgorithms =
        "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa";

    ssh_options_set(session.get(), SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(session.get(), SSH_OPTIONS_PORT, &port);
    ssh_options_set(session.get(), SSH_OPTIONS_USER, user.constData());
    ssh_options_set(session.get(), SSH_OPTIONS_KNOWNHOSTS,
                    knownHosts.constData());
    ssh_options_set(session.get(), SSH_OPTIONS_TIMEOUT, &timeout);
    ssh_options_set(session.get(), SSH_OPTIONS_HOSTKEYS, hostKeyAlgorithms);

    if (ssh_connect(session.get()) != SSH_OK) {
        postError(generation, sessionError(session.get(),
            tr("SFTP SSH connection failed")));
        return;
    }

    // SFTP 只在对应终端已完成主机密钥确认后启动；独立连接必须得到相同结果。
    // 若密钥此时未知或发生变化，拒绝继续，绝不在后台静默信任。
    if (ssh_session_is_known_server(session.get()) != SSH_KNOWN_HOSTS_OK) {
        postError(generation,
            tr("The SFTP host key is not trusted or has changed. "
               "Reconnect the SSH terminal and verify the host key."));
        ssh_disconnect(session.get());
        return;
    }

    QString authenticationError;
    if (!authenticate(session.get(), config, authenticationError)) {
        postError(generation, authenticationError);
        ssh_disconnect(session.get());
        return;
    }

    SftpSessionPtr sftp{sftp_new(session.get()), &sftp_free};
    if (!sftp || sftp_init(sftp.get()) != SSH_OK) {
        postError(generation, sessionError(session.get(),
            tr("Failed to initialize SFTP")));
        ssh_disconnect(session.get());
        return;
    }

    char* canonicalHome = sftp_canonicalize_path(sftp.get(), ".");
    const QString homePath = canonicalHome
        ? QString::fromUtf8(canonicalHome) : QStringLiteral(".");
    if (canonicalHome)
        ssh_string_free_char(canonicalHome);

    _connected.store(true, std::memory_order_release);
    QMetaObject::invokeMethod(this, [this, generation, homePath]() {
        if (_generation == generation)
            emit connected(homePath);
    }, Qt::QueuedConnection);

    while (_running.load(std::memory_order_acquire)
           && ssh_is_connected(session.get())) {
        Command command;
        {
            QMutexLocker lock(&_queueMutex);
            while (_commands.isEmpty()
                   && _running.load(std::memory_order_acquire)
                   && ssh_is_connected(session.get())) {
                _queueReady.wait(&_queueMutex, 200);
            }
            if (!_running.load(std::memory_order_acquire)
                || !ssh_is_connected(session.get())) {
                break;
            }
            command = _commands.dequeue();
        }

        if (command.type == CommandType::List) {
            const QByteArray requested = command.source.toUtf8();
            char* canonical =
                sftp_canonicalize_path(sftp.get(), requested.constData());
            if (!canonical) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Cannot resolve the remote directory")));
                continue;
            }
            const QString directoryPath = QString::fromUtf8(canonical);
            ssh_string_free_char(canonical);
            const QByteArray encodedPath = directoryPath.toUtf8();
            SftpDirectoryPtr directory{
                sftp_opendir(sftp.get(), encodedPath.constData()),
                &sftp_closedir};
            if (!directory) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Cannot open remote directory %1").arg(directoryPath)));
                continue;
            }

            QVector<SftpFileInfo> entries;
            for (;;) {
                SftpAttributesPtr attributes{
                    sftp_readdir(sftp.get(), directory.get()),
                    &sftp_attributes_free};
                if (!attributes)
                    break;
                const QString name = QString::fromUtf8(
                    attributes->name ? attributes->name : "");
                if (name == QStringLiteral(".")
                    || name == QStringLiteral("..")) {
                    continue;
                }

                SftpFileInfo info;
                info.name = name;
                info.path = remotePathJoin(directoryPath, name);
                info.size = attributes->size;
                info.modifiedSeconds = static_cast<qint64>(
                    attributes->mtime64 != 0
                        ? attributes->mtime64 : attributes->mtime);
                info.permissions = attributes->permissions;
                info.directory =
                    attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
                info.symbolicLink =
                    attributes->type == SSH_FILEXFER_TYPE_SYMLINK;
                info.hardLink = isHardLinkFromLongName(attributes.get());
                entries.push_back(std::move(info));
            }
            if (!sftp_dir_eof(directory.get())) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Failed while reading remote directory %1")
                        .arg(directoryPath)));
                continue;
            }

            QMetaObject::invokeMethod(
                this, [this, generation, directoryPath,
                       entries = std::move(entries)]() {
                    if (_generation == generation)
                        emit directoryListed(directoryPath, entries);
                }, Qt::QueuedConnection);
            continue;
        }

        if (command.type == CommandType::UploadDirectory) {
            const QFileInfo rootInfo(command.source);
            if (!rootInfo.exists() || !rootInfo.isDir()
                || rootInfo.isSymLink()) {
                postError(generation,
                    tr("Local path is not a directory: %1")
                        .arg(command.source));
                continue;
            }

            const QDir rootDirectory(rootInfo.absoluteFilePath());
            const QDir::Filters filters = QDir::AllEntries
                | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;
            quint64 total = 0;
            QDirIterator sizeIterator(
                rootInfo.absoluteFilePath(), filters,
                QDirIterator::Subdirectories);
            while (sizeIterator.hasNext()) {
                sizeIterator.next();
                const QFileInfo info = sizeIterator.fileInfo();
                if (!info.isSymLink() && info.isFile())
                    total += static_cast<quint64>(info.size());
            }

            QString error;
            bool succeeded = ensureRemoteDirectory(
                sftp.get(), session.get(), command.target, error);
            QSet<QString> createdDirectories{command.target};
            quint64 transferred = 0;
            quint64 lastReported = 0;
            const auto reportProgress =
                [this, generation, path = command.target](
                    quint64 current, quint64 totalBytes) {
                QMetaObject::invokeMethod(this,
                    [this, generation, path, current, totalBytes]() {
                        if (_generation == generation) {
                            emit transferProgress(
                                path, current, totalBytes);
                        }
                    }, Qt::QueuedConnection);
            };

            QDirIterator iterator(rootInfo.absoluteFilePath(), filters,
                                  QDirIterator::Subdirectories);
            while (succeeded && iterator.hasNext()
                   && _running.load(std::memory_order_acquire)) {
                iterator.next();
                const QFileInfo info = iterator.fileInfo();
                // 上传目录时不跟随本地软链接，避免形成递归环。
                if (info.isSymLink())
                    continue;
                QString relativePath = rootDirectory.relativeFilePath(
                    info.absoluteFilePath());
                relativePath.replace(QLatin1Char('\\'), QLatin1Char('/'));
                const QStringList parts = relativePath.split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
                if (parts.isEmpty())
                    continue;

                QString parentPath = command.target;
                for (qsizetype index = 0;
                     index + 1 < parts.size(); ++index) {
                    parentPath = remotePathJoin(
                        parentPath, parts.at(index));
                    if (!createdDirectories.contains(parentPath)) {
                        succeeded = ensureRemoteDirectory(
                            sftp.get(), session.get(), parentPath, error);
                        if (!succeeded)
                            break;
                        createdDirectories.insert(parentPath);
                    }
                }
                if (!succeeded)
                    break;

                const QString remotePath = remotePathJoin(
                    parentPath, parts.constLast());
                if (info.isDir()) {
                    succeeded = ensureRemoteDirectory(
                        sftp.get(), session.get(), remotePath, error);
                    if (succeeded)
                        createdDirectories.insert(remotePath);
                } else if (info.isFile()) {
                    succeeded = uploadRegularFile(
                        sftp.get(), session.get(), info.absoluteFilePath(),
                        remotePath, _running, transferred, total,
                        lastReported, reportProgress, error);
                }
            }
            if (!succeeded) {
                if (_running.load(std::memory_order_acquire))
                    postError(generation, error);
                continue;
            }
            if (_running.load(std::memory_order_acquire)) {
                QMetaObject::invokeMethod(this,
                    [this, generation, path = command.target]() {
                        if (_generation == generation) {
                            emit operationFinished(
                                QStringLiteral("upload"), path);
                        }
                    }, Qt::QueuedConnection);
            }
            continue;
        }

        if (command.type == CommandType::Upload) {
            QFile localFile(command.source);
            if (!localFile.open(QIODevice::ReadOnly)) {
                postError(generation,
                    tr("Cannot open local file %1: %2")
                        .arg(command.source, localFile.errorString()));
                continue;
            }
            const QByteArray remotePath = command.target.toUtf8();
            // sftp_open() 的 accesstype 是本机 POSIX O_* 标志，libssh 会在内部
            // 转换为 SSH_FXF_*；直接传协议常量在 Windows 上会产生错误访问模式。
            SftpFilePtr remoteFile{
                sftp_open(sftp.get(), remotePath.constData(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644),
                &sftp_close};
            if (!remoteFile) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Cannot open remote file %1").arg(command.target)));
                continue;
            }

            const quint64 total = static_cast<quint64>(localFile.size());
            quint64 transferred = 0;
            quint64 lastReported = 0;
            bool failed = false;
            while (_running.load(std::memory_order_acquire)) {
                const QByteArray chunk = localFile.read(64 * 1024);
                if (chunk.isEmpty()) {
                    if (localFile.error() != QFileDevice::NoError) {
                        postError(generation, tr("Failed to read local file %1: %2")
                            .arg(command.source, localFile.errorString()));
                        failed = true;
                    }
                    break;
                }
                qsizetype offset = 0;
                while (offset < chunk.size()
                       && _running.load(std::memory_order_acquire)) {
                    const auto written = sftp_write(
                        remoteFile.get(), chunk.constData() + offset,
                        static_cast<size_t>(chunk.size() - offset));
                    if (written <= 0) {
                        postError(generation, sftpError(sftp.get(), session.get(),
                            tr("Failed to upload %1").arg(command.target)));
                        failed = true;
                        break;
                    }
                    offset += static_cast<qsizetype>(written);
                    transferred += static_cast<quint64>(written);
                    // 限制进度事件频率，避免高速传输用大量 queued event 淹没 GUI。
                    if (transferred - lastReported >= 256 * 1024
                        || transferred == total) {
                        lastReported = transferred;
                        QMetaObject::invokeMethod(this,
                            [this, generation, path = command.target,
                             transferred, total]() {
                                if (_generation == generation)
                                    emit transferProgress(path, transferred, total);
                            }, Qt::QueuedConnection);
                    }
                }
                if (failed)
                    break;
            }
            if (!failed && _running.load(std::memory_order_acquire)
                && sftp_close(remoteFile.release()) != SSH_OK) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Failed to finalize remote file %1")
                        .arg(command.target)));
                failed = true;
            }
            if (!failed && _running.load(std::memory_order_acquire)) {
                QMetaObject::invokeMethod(this,
                    [this, generation, path = command.target]() {
                        if (_generation == generation)
                            emit operationFinished(QStringLiteral("upload"), path);
                    }, Qt::QueuedConnection);
            }
            continue;
        }

        if (command.type == CommandType::DownloadDirectory) {
            QVector<RemoteDownloadEntry> entries;
            quint64 total = 0;
            QString error;
            bool succeeded = collectRemoteDirectory(
                sftp.get(), session.get(), command.source,
                entries, total, _running, error);
            if (succeeded && !QDir().mkpath(command.target)) {
                error = tr("Cannot create local directory %1")
                            .arg(command.target);
                succeeded = false;
            }

            quint64 transferred = 0;
            quint64 lastReported = 0;
            const auto reportProgress =
                [this, generation, path = command.source](
                    quint64 current, quint64 totalBytes) {
                QMetaObject::invokeMethod(this,
                    [this, generation, path, current, totalBytes]() {
                        if (_generation == generation) {
                            emit transferProgress(
                                path, current, totalBytes);
                        }
                    }, Qt::QueuedConnection);
            };
            const QDir localRoot(command.target);
            for (const RemoteDownloadEntry& entry : entries) {
                if (!succeeded
                    || !_running.load(std::memory_order_acquire)) {
                    break;
                }
                const QString localPath = QDir::cleanPath(
                    localRoot.absoluteFilePath(entry.relativePath));
                if (entry.directory) {
                    if (!QDir().mkpath(localPath)) {
                        error = tr("Cannot create local directory %1")
                                    .arg(localPath);
                        succeeded = false;
                    }
                    continue;
                }
                if (!QDir().mkpath(QFileInfo(localPath).absolutePath())) {
                    error = tr("Cannot create local directory %1")
                                .arg(QFileInfo(localPath).absolutePath());
                    succeeded = false;
                    break;
                }
                succeeded = downloadRegularFile(
                    sftp.get(), session.get(), entry.remotePath, localPath,
                    _running, transferred, total, lastReported,
                    reportProgress, error);
            }
            if (!succeeded) {
                if (_running.load(std::memory_order_acquire))
                    postError(generation, error);
                continue;
            }
            if (_running.load(std::memory_order_acquire)) {
                QMetaObject::invokeMethod(this,
                    [this, generation, path = command.source]() {
                        if (_generation == generation) {
                            emit operationFinished(
                                QStringLiteral("download"), path);
                        }
                    }, Qt::QueuedConnection);
            }
            continue;
        }

        if (command.type == CommandType::Download) {
            const QByteArray remotePath = command.source.toUtf8();
            SftpFilePtr remoteFile{
                sftp_open(sftp.get(), remotePath.constData(), O_RDONLY, 0),
                &sftp_close};
            if (!remoteFile) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Cannot open remote file %1").arg(command.source)));
                continue;
            }
            SftpAttributesPtr attributes{
                sftp_fstat(remoteFile.get()), &sftp_attributes_free};
            const quint64 total = attributes ? attributes->size : 0;
            QSaveFile localFile(command.target);
            if (!localFile.open(QIODevice::WriteOnly)) {
                postError(generation,
                    tr("Cannot create local file %1: %2")
                        .arg(command.target, localFile.errorString()));
                continue;
            }

            QByteArray buffer(64 * 1024, Qt::Uninitialized);
            quint64 transferred = 0;
            quint64 lastReported = 0;
            bool failed = false;
            while (_running.load(std::memory_order_acquire)) {
                const auto bytesRead = sftp_read(
                    remoteFile.get(), buffer.data(),
                    static_cast<size_t>(buffer.size()));
                if (bytesRead == 0)
                    break;
                if (bytesRead < 0) {
                    postError(generation, sftpError(sftp.get(), session.get(),
                        tr("Failed to download %1").arg(command.source)));
                    failed = true;
                    break;
                }
                if (localFile.write(buffer.constData(), bytesRead) != bytesRead) {
                    postError(generation,
                        tr("Failed to write local file %1: %2")
                            .arg(command.target, localFile.errorString()));
                    failed = true;
                    break;
                }
                transferred += static_cast<quint64>(bytesRead);
                if (transferred - lastReported >= 256 * 1024
                    || transferred == total) {
                    lastReported = transferred;
                    QMetaObject::invokeMethod(this,
                        [this, generation, path = command.source,
                         transferred, total]() {
                            if (_generation == generation)
                                emit transferProgress(path, transferred, total);
                        }, Qt::QueuedConnection);
                }
            }
            if (!failed && _running.load(std::memory_order_acquire)
                && localFile.commit()) {
                QMetaObject::invokeMethod(this,
                    [this, generation, path = command.source]() {
                        if (_generation == generation)
                            emit operationFinished(QStringLiteral("download"), path);
                    }, Qt::QueuedConnection);
            } else if (!failed && _running.load(std::memory_order_acquire)) {
                postError(generation,
                    tr("Failed to finalize local file %1: %2")
                        .arg(command.target, localFile.errorString()));
            }
            continue;
        }

        const QByteArray source = command.source.toUtf8();

        if (command.type == CommandType::DeleteRemoteDirectory) {
            QString error;
            if (!removeRemoteDirectoryRecursively(
                    sftp.get(), session.get(), command.source,
                    _running, error)) {
                if (_running.load(std::memory_order_acquire))
                    postError(generation, error);
                continue;
            }
            QMetaObject::invokeMethod(this,
                [this, generation, path = command.source]() {
                    if (_generation == generation) {
                        emit operationFinished(
                            QStringLiteral("remove"), path);
                    }
                }, Qt::QueuedConnection);
            continue;
        }

        if (command.type == CommandType::CreateRemoteFile) {
            // O_EXCL 防止误覆盖远端同名文件，新建文件默认使用常见的 0644 权限。
            SftpFilePtr remoteFile{
                sftp_open(sftp.get(), source.constData(),
                          O_WRONLY | O_CREAT | O_EXCL, 0644),
                &sftp_close};
            if (!remoteFile) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Cannot create remote file %1").arg(command.source)));
                continue;
            }
            if (sftp_close(remoteFile.release()) != SSH_OK) {
                postError(generation, sftpError(sftp.get(), session.get(),
                    tr("Failed to finalize remote file %1")
                        .arg(command.source)));
                continue;
            }
            QMetaObject::invokeMethod(this,
                [this, generation, path = command.source]() {
                    if (_generation == generation) {
                        emit operationFinished(
                            QStringLiteral("create-file"), path);
                    }
                }, Qt::QueuedConnection);
            continue;
        }

        bool succeeded = false;
        QString operation;
        if (command.type == CommandType::MakeRemoteDirectory) {
            operation = QStringLiteral("mkdir");
            succeeded = sftp_mkdir(sftp.get(), source.constData(), 0755) == SSH_OK;
        } else if (command.type == CommandType::ChangePermissions) {
            operation = QStringLiteral("chmod");
            succeeded = sftp_chmod(sftp.get(), source.constData(),
                                   command.permissions) == SSH_OK;
        } else if (command.type == CommandType::RemoveFile) {
            operation = QStringLiteral("remove");
            succeeded = sftp_unlink(sftp.get(), source.constData()) == SSH_OK;
        } else if (command.type == CommandType::Rename) {
            operation = QStringLiteral("rename");
            const QByteArray target = command.target.toUtf8();
            succeeded = sftp_rename(sftp.get(), source.constData(),
                                    target.constData()) == SSH_OK;
        }

        if (!succeeded) {
            postError(generation, sftpError(sftp.get(), session.get(),
                tr("Remote operation failed for %1").arg(command.source)));
            continue;
        }
        QMetaObject::invokeMethod(this,
            [this, generation, operation, path = command.source]() {
                if (_generation == generation)
                    emit operationFinished(operation, path);
            }, Qt::QueuedConnection);
    }

    // SFTP channel 必须先于底层 SSH session 关闭。
    sftp.reset();
    ssh_disconnect(session.get());
}
