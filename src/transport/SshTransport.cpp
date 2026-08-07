#include "SshTransport.h"

#include <libssh/libssh.h>

#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMetaObject>

#include <utility>

namespace {

QString defaultKnownHostsPath()
{
    // 使用用户目录下的 OpenSSH known_hosts，与系统 ssh 客户端共享信任。
    return QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
}

} // namespace

SshTransport::SshTransport(SshConfig config, QObject* parent)
    : ITransport(parent)
    , _config(std::move(config))
    , _keepAliveMs(_config.keepAliveSeconds > 0 ? _config.keepAliveSeconds * 1000 : 0)
{
    // 静态链接 libssh 必须显式初始化（共享库由 DllMain 自动做）。
    // ssh_init()/ssh_finalize() 内部带引用计数，多个实例安全配对。
    ssh_init();
}

SshTransport::~SshTransport()
{
    disconnect();
    ssh_finalize();
}

bool SshTransport::connectToHost()
{
    disconnect();

    if (!_config.isValid()) {
        reportError(tr("Invalid SSH configuration."));
        return false;
    }

    // 确保 known_hosts 所在目录存在，否则首次信任写入会失败。
    const QString kh = defaultKnownHostsPath();
    QDir().mkpath(QFileInfo(kh).absolutePath());

    _running.store(true);
    _connected.store(false);
    _readPaused.store(false);
    // 注意：不重置 _pendingCols/_pendingRows —— attachTransport 在
    // connectToHost() 之前已通过 resizeTerminal() 写入当前终端尺寸，
    // worker 打开 channel 时以它作为初始 PTY 尺寸。

    _thread = QThread::create([this]() { workerMain(); });
    _thread->setObjectName(QStringLiteral("SshTransport"));
    _thread->start();
    return true;
}

void SshTransport::disconnect()
{
    _running.store(false);
    _readPaused.store(false, std::memory_order_release);

    // 唤醒可能阻塞在主机密钥决策上的工作线程。
    {
        QMutexLocker lock(&_keyMutex);
        _keyDecision = 0;
        _keyWait.wakeAll();
    }

    _connected.store(false);

    if (_thread) {
        // 事件循环每 20ms 检查一次 _running，正常会话 1s 内即可回收；
        // 上限覆盖 ssh_connect（10s 连接超时）最坏场景。
        _thread->wait(TeardownWaitMs);
        delete _thread;
        _thread = nullptr;
    }
}

void SshTransport::write(const QByteArray& data)
{
    if (data.isEmpty() || !_running.load(std::memory_order_acquire))
        return;

    QMutexLocker lock(&_writeMutex);
    if (_writeQueue.size() + data.size() > MaxPendingWriteBytes) {
        reportError(tr("SSH write queue exceeded its 1 MiB limit."));
        return;
    }
    _writeQueue.append(data);
}

void SshTransport::resizeTerminal(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    _pendingCols.store(cols);
    _pendingRows.store(rows);
}

bool SshTransport::isConnected() const
{
    return _connected.load(std::memory_order_acquire);
}

QString SshTransport::errorString() const
{
    QMutexLocker lock(&_errorMutex);
    return _errorString;
}

bool SshTransport::setReadPaused(bool paused)
{
    _readPaused.store(paused, std::memory_order_release);
    return true;
}

void SshTransport::acceptHostKey()
{
    QMutexLocker lock(&_keyMutex);
    _keyDecision = 1;
    _keyWait.wakeAll();
}

void SshTransport::rejectHostKey()
{
    QMutexLocker lock(&_keyMutex);
    _keyDecision = 0;
    _keyWait.wakeAll();
}

void SshTransport::reportError(const QString& message)
{
    {
        QMutexLocker lock(&_errorMutex);
        _errorString = message;
    }
    QMetaObject::invokeMethod(this, [this, message]() {
        emit errorOccurred(message);
    }, Qt::QueuedConnection);
}

void SshTransport::emitReadyRead(const QByteArray& data)
{
    // 把数据投递回 GUI 线程再发信号，避免跨线程直连。
    QMetaObject::invokeMethod(this, [this, data]() {
        emit readyRead(data);
    }, Qt::QueuedConnection);
}

void SshTransport::emitSignal(void (SshTransport::*signal)())
{
    QMetaObject::invokeMethod(this, [this, signal]() {
        emit (this->*signal)();
    }, Qt::QueuedConnection);
}

void SshTransport::workerMain()
{
    ssh_session session = ssh_new();
    if (!session) {
        reportError(tr("Failed to create SSH session."));
        return;
    }

    const QByteArray host = _config.host.trimmed().toUtf8();
    const QByteArray user = _config.username.trimmed().toUtf8();
    const QByteArray term = _config.terminalType.trimmed().isEmpty()
        ? QByteArrayLiteral("xterm-256color")
        : _config.terminalType.trimmed().toUtf8();

    int port = static_cast<int>(_config.port);
    long timeoutSec = ConnectTimeoutSec;
    const QByteArray knownHosts =
        QDir::toNativeSeparators(defaultKnownHostsPath()).toUtf8();
    const char* hostKeyAlgorithms =
        "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa";

    ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData());
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);
    ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, hostKeyAlgorithms);

    // ── 连接 ─────────────────────────────────────────────
    if (ssh_connect(session) != SSH_OK) {
        reportError(tr("SSH connection to %1:%2 failed: %3")
                        .arg(_config.host)
                        .arg(_config.port)
                        .arg(QString::fromUtf8(ssh_get_error(session))));
        ssh_free(session);
        return;
    }

    // ── 主机密钥验证（必须经过 UI 决策，绝不静默接受）──
    ssh_key serverKey = nullptr;
    if (ssh_get_server_publickey(session, &serverKey) != SSH_OK) {
        reportError(tr("Failed to retrieve the server host key: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    const enum ssh_known_hosts_e knownState = ssh_session_is_known_server(session);
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        reportError(tr("Cannot read known_hosts file %1: %2")
                        .arg(QString::fromUtf8(knownHosts),
                             QString::fromUtf8(ssh_get_error(session))));
        ssh_key_free(serverKey);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    if (knownState != SSH_KNOWN_HOSTS_OK) {
        SshHostKeyInfo info;
        info.host = _config.host;
        info.port = _config.port;
        info.status = (knownState == SSH_KNOWN_HOSTS_CHANGED
                       || knownState == SSH_KNOWN_HOSTS_OTHER)
            ? SshHostKeyStatus::Changed
            : SshHostKeyStatus::New;

        const char* type = ssh_key_type_to_char(ssh_key_type(serverKey));
        info.keyType = QString::fromUtf8(type ? type : "");

        unsigned char* hash = nullptr;
        size_t hashLen = 0;
        if (ssh_get_publickey_hash(serverKey, SSH_PUBLICKEY_HASH_SHA256,
                                   &hash, &hashLen) == SSH_OK
            && hash) {
            char* hex = ssh_get_hexa(hash, hashLen);
            info.fingerprint = QString::fromLatin1(hex ? hex : "");
            if (hex)
                ssh_string_free_char(hex);
            ssh_clean_pubkey_hash(&hash);
        }

        // 请求 UI 决策，然后在此线程上等待（带超时，可被 disconnect 唤醒）。
        _keyDecision = -1;
        QMetaObject::invokeMethod(this, [this, info]() {
            emit hostKeyRequired(info);
        }, Qt::QueuedConnection);

        {
            QMutexLocker lock(&_keyMutex);
            while (_running.load(std::memory_order_acquire) && _keyDecision < 0)
                _keyWait.wait(&_keyMutex, 100);
        }

        if (!_running.load(std::memory_order_acquire)) {
            // disconnect() 已请求中止。
            ssh_key_free(serverKey);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
        if (_keyDecision != 1) {
            reportError(tr("Host key verification failed; connection aborted."));
            ssh_key_free(serverKey);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }

        // 信任并写入 known_hosts（New 追加，Changed 更新）。
        if (ssh_write_knownhost(session) != SSH_OK) {
            reportError(tr("Failed to store the host key: %1")
                            .arg(QString::fromUtf8(ssh_get_error(session))));
            ssh_key_free(serverKey);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
    }
    ssh_key_free(serverKey);

    // ── 认证 ─────────────────────────────────────────────
    int authResult = SSH_AUTH_ERROR;
    if (_config.authMethod == QStringLiteral("publickey")) {
        ssh_key privkey = nullptr;
        const QByteArray keyPath =
            QDir::toNativeSeparators(_config.privateKeyPath.trimmed()).toUtf8();
        const QByteArray passphrase = _config.keyPassphrase.toUtf8();
        if (ssh_pki_import_privkey_file(
                keyPath.constData(),
                passphrase.isEmpty() ? nullptr : passphrase.constData(),
                nullptr, nullptr, &privkey) != SSH_OK
            || !privkey) {
            reportError(tr("Failed to load private key %1: %2")
                            .arg(_config.privateKeyPath,
                                 QString::fromUtf8(ssh_get_error(session))));
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
        authResult = ssh_userauth_publickey(session, user.constData(), privkey);
        ssh_key_free(privkey);
        if (authResult != SSH_AUTH_SUCCESS) {
            reportError(tr("Public key authentication failed for %1@%2: %3")
                            .arg(_config.username, _config.host,
                                 QString::fromUtf8(ssh_get_error(session))));
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
    } else {
        const QByteArray pass = _config.password.toUtf8();
        authResult =
            ssh_userauth_password(session, user.constData(), pass.constData());
        if (authResult != SSH_AUTH_SUCCESS) {
            reportError(tr("Password authentication failed for %1@%2: %3")
                            .arg(_config.username, _config.host,
                                 QString::fromUtf8(ssh_get_error(session))));
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
    }

    // ── 打开 channel：PTY + shell ────────────────────────
    ssh_channel channel = ssh_channel_new(session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
        reportError(tr("Failed to open SSH channel: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
        if (channel)
            ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    const int startCols = _pendingCols.load() > 0 ? _pendingCols.load() : 80;
    const int startRows = _pendingRows.load() > 0 ? _pendingRows.load() : 24;

    if (ssh_channel_request_pty_size(channel, term.constData(),
                                     startCols, startRows) != SSH_OK
        || ssh_channel_request_shell(channel) != SSH_OK) {
        reportError(tr("Failed to start remote shell: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    ssh_event event = ssh_event_new();
    if (!event) {
        reportError(tr("Failed to create SSH event loop."));
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }
    ssh_event_add_session(event, session);

    _connected.store(true);
    emitSignal(&SshTransport::connected);

    // ── 事件循环：IO / resize / keepalive / 断线检测 ─────
    QElapsedTimer keepaliveTimer;
    keepaliveTimer.start();
    int appliedCols = startCols;
    int appliedRows = startRows;

    while (_running.load(std::memory_order_acquire)
           && channel
           && ssh_is_connected(session)) {
        ssh_event_dopoll(event, 20);

        // drain 对端数据（暂停时仍轮询协议，避免 SSH 窗口/流控死锁）
        if (!_readPaused.load(std::memory_order_acquire)) {
            for (;;) {
                char buf[65536];
                const int n =
                    ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
                if (n > 0) {
                    emitReadyRead(QByteArray(buf, n));
                } else if (n == 0) {
                    break;   // 当前无数据
                } else if (ssh_channel_is_eof(channel)) {
                    _running.store(false);
                    break;
                } else {
                    reportError(tr("SSH channel read error: %1")
                                    .arg(QString::fromUtf8(
                                        ssh_get_error(session))));
                    _running.store(false);
                    break;
                }
            }
        }

        // 消费 GUI 线程提交的用户输入
        QByteArray toWrite;
        {
            QMutexLocker lock(&_writeMutex);
            toWrite.swap(_writeQueue);
        }
        if (!toWrite.isEmpty()) {
            const char* p = toWrite.constData();
            int remain = toWrite.size();
            while (remain > 0 && _running.load() && ssh_is_connected(session)) {
                const int n =
                    ssh_channel_write(channel, p, static_cast<uint32_t>(remain));
                if (n <= 0) {
                    reportError(tr("SSH channel write failed: %1")
                                    .arg(QString::fromUtf8(
                                        ssh_get_error(session))));
                    break;
                }
                p += n;
                remain -= n;
            }
        }

        // 应用窗口尺寸变更
        const int pc = _pendingCols.load();
        const int pr = _pendingRows.load();
        if (pc > 0 && pr > 0 && (pc != appliedCols || pr != appliedRows)) {
            // resize 必须发 window-change（RFC 4254 §6.7），不能重复 pty-req：
            // 服务器对已分配 PTY 的通道再次收到 pty-req 会回 CHANNEL_FAILURE，
            // 报 "Channel request pty-req failed on channel N:0"。
            // ssh_channel_change_pty_size 发 want_reply=0 的通知，不阻塞事件循环。
            if (ssh_channel_change_pty_size(channel, pc, pr) != SSH_OK) {
                reportError(tr("Failed to resize the remote PTY: %1")
                                .arg(QString::fromUtf8(ssh_get_error(session))));
            }
            appliedCols = pc;
            appliedRows = pr;
            _pendingCols.store(-1);
            _pendingRows.store(-1);
        }

        // keepalive：发送 SSH_MSG_IGNORE 保活（0 表示禁用）
        if (_keepAliveMs > 0 && keepaliveTimer.elapsed() >= _keepAliveMs) {
            ssh_send_ignore(session, "");
            keepaliveTimer.restart();
        }

        if (ssh_channel_is_eof(channel) || !ssh_is_connected(session))
            _running.store(false);
    }

    // ── 关闭 ─────────────────────────────────────────────
    const bool wasConnected = _connected.exchange(false);

    if (channel) {
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (event)
        ssh_event_free(event);
    if (ssh_is_connected(session))
        ssh_disconnect(session);
    ssh_free(session);

    if (wasConnected)
        emitSignal(&SshTransport::disconnected);
}
