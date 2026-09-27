/**
 * @file   SshTransport.cpp
 * @brief  SSH 传输实现：连接认证阻塞、channel 非阻塞的工作线程封装。
 *
 * 工作线程按阶段推进：连接 → 主机密钥验证（需 GUI 决策）→ 认证
 * → 打开 channel 并请求 PTY+shell → 进入事件循环（IO/resize/keepalive）
 * → 关闭。GUI 线程仅通过原子量与互斥队列与工作线程交互。
 */
#include "SshWorkerWakeup.h"
#include "SshTransport.h"
#include "SshCommandCompletion.h"
#include "SshMonitorProtocol.h"

#include "core/ThreadNaming.h"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/options.h>

#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <climits>

#include <utility>
#include <algorithm>

namespace {

// SSH 会话工作线程名。QThread::objectName 与 OS 级线程名共用同一字符串，
// 避免两处名字漂移：Qt 只在 Linux/Unix 上把 objectName 写进内核线程名，
// Windows release 构建下仍需 worker 入口显式设置。
constexpr char SshWorkerThreadName[] = "nvterm-ssh";

QByteArray resourceMonitorCommand()
{
    // 常驻 shell 阻塞等待请求；只传原始文本，计算留在 NovaTerm。
    return QByteArrayLiteral(R"NOVATERM(LC_ALL=C; export LC_ALL
while IFS= read -r nt_request; do
case "$nt_request" in *[!0-9]*|'') continue;; esac
printf '__NOVATERM_METRICS_BEGIN__\t%s\n' "$nt_request"
printf '@@stat\n'
IFS= read -r nt_cpu < /proc/stat; printf '%s\n' "$nt_cpu"
printf '@@meminfo\n'
cat /proc/meminfo /proc/net/dev 2>/dev/null
printf '\n'
for nt_file in loadavg uptime; do
  printf '@@%s\n' "$nt_file"
  IFS= read -r nt_value < "/proc/$nt_file"
  printf '%s\n' "$nt_value"
done
printf '__NOVATERM_METRICS_END__\t%s\n' "$nt_request"
done
)NOVATERM");
}

QString defaultKnownHostsPath()
{
    // 使用用户目录下的 OpenSSH known_hosts，与系统 ssh 客户端共享信任。
    return QDir::homePath() + QStringLiteral("/.ssh/known_hosts");
}

void commandExitStatusCallback(ssh_session, ssh_channel, int exitStatus,
                               void* userData)
{
    auto* const completion = static_cast<SshCommandCompletion*>(userData);
    completion->observeExitStatus(exitStatus);
}

} // namespace

SshTransport::SshTransport(SshConfig config, QObject* parent)
    : ITransport(parent)
    , _config(std::move(config))
    , _knownHostsPath(defaultKnownHostsPath())
    , _keepAliveMs(_config.keepAliveSeconds > 0 ? _config.keepAliveSeconds * 1000 : 0)
    , _wakeup(std::make_unique<SshWorkerWakeup>())
{
    // 静态链接 libssh 必须显式初始化（共享库由 DllMain 自动做）。
    // ssh_init()/ssh_finalize() 内部带引用计数，多个实例安全配对。
    ssh_init();
}

SshTransport::~SshTransport()
{
    // 析构在即：成员即将失效，仍在运行的工作线程访问它们就是 UAF。
    // 比交互路径更有耐心，等满阻塞调用串行的最坏场景。
    disconnectInternal(TeardownDestructorWaitMs);
    ssh_finalize();
}

bool SshTransport::connectToHost()
{
    disconnect();

    if (_workerAbandoned) {
        reportError(tr("The previous SSH session is still shutting down; "
                       "please try again shortly."),
                    TransportErrorCategory::Unknown);
        return false;
    }

    if (!_wakeup->valid()) {
        reportError(QStringLiteral("Cannot create SSH worker wakeup socket."),
                    TransportErrorCategory::Io);
        return false;
    }

    if (!_config.isValid()) {
        reportError(tr("Invalid SSH configuration."),
                    TransportErrorCategory::Configuration);
        return false;
    }

    // 确保 known_hosts 所在目录存在，否则首次信任写入会失败。
    const QString kh = _knownHostsPath;
    QDir().mkpath(QFileInfo(kh).absolutePath());

    _running.store(true);
    _connected.store(false);
    _readPaused.store(false);
    {
        // 新连接不得继承上一代会话尚未执行的辅助命令，避免命令被发往错误主机。
        QMutexLocker lock(&_commandMutex);
        _commandQueue.clear();
        _cancelCommandRequestId = 0;
        _commandActive.store(false, std::memory_order_release);
    }
    {
        QMutexLocker lock(&_monitorMutex);
        _monitorEnabled = false;
        ++_monitorGeneration;
        _monitorRequestId = 0;
    }
    // 注意：不重置 _pendingCols/_pendingRows —— attachTransport 在
    // connectToHost() 之前已通过 resizeTerminal() 写入当前终端尺寸，
    // worker 打开 channel 时以它作为初始 PTY 尺寸。

    _thread = QThread::create([this]() { workerMain(); });
    _thread->setObjectName(QString::fromLatin1(SshWorkerThreadName));
    _thread->start();
    return true;
}

void SshTransport::disconnect()
{
    disconnectInternal(TeardownWaitMs);
}

void SshTransport::disconnectInternal(int waitMs)
{
    _connectionGeneration.fetch_add(1, std::memory_order_acq_rel);
    _running.store(false);
    _wakeup->notify();
    _readPaused.store(false, std::memory_order_release);

    // 唤醒可能阻塞在主机密钥决策上的工作线程。
    {
        QMutexLocker lock(&_keyMutex);
        _keyDecision = 0;
        _keyWait.wakeAll();
    }

    const bool wasConnected = _connected.exchange(false);
    {
        // 整体赋值而非 clear()：clear 保留容量，经历过一次 1 MiB 峰值后
        // 这块堆内存会驻留到对象析构。disconnect 是冷路径，realloc 无关
        // 性能；热路径（deliverInbound 全消费处）保持 clear 不动。
        QMutexLocker lock(&_inboundMutex);
        ++_inboundGeneration;
        _inbound = QByteArray{};
        _inboundHead = 0;
        _inboundScheduled = false;
        _inboundClosed = false;
    }

    {
        // 丢弃尚未开始的请求；正在执行的 channel 由工作线程退出路径统一回收。
        QMutexLocker lock(&_commandMutex);
        for (const auto& request : std::as_const(_commandQueue)) {
            if (request.bounded) {
                CommandExecutionResult result;
                result.requestId = request.requestId;
                result.connectionGeneration = request.generation;
                result.outcome = CommandExecutionOutcome::Disconnected;
                emitBoundedCommandFinished(std::move(result));
            }
        }
        _commandQueue.clear();
        _cancelCommandRequestId = 0;
    }
    {
        QMutexLocker lock(&_monitorMutex);
        _monitorEnabled = false;
        ++_monitorGeneration;
        _monitorRequestId = 0;
    }

    if (_thread) {
        // 唤醒事件循环立即检查 _running；waitMs 需覆盖连接+认证等
        // 阻塞调用串行的最坏场景（每步以 ConnectTimeoutSec 为上限）。
        if (_thread->wait(waitMs)) {
            delete _thread;
        } else {
            // 工作线程仍卡在不可中止的阻塞调用里。绝不能 delete 仍在
            // 运行的 QThread（UB）：让它结束后自毁，并标记本对象不可再
            // 启动新会话 —— 僵尸线程可能仍在访问成员。
            qWarning("SshTransport: worker did not stop within %d ms; "
                     "deferring thread teardown", waitMs);
            _thread->disconnect();
            QObject::connect(_thread, &QThread::finished,
                             _thread, &QObject::deleteLater);
            _workerAbandoned = true;
        }
        _thread = nullptr;
    }
    {
        QMutexLocker lock(&_inboundMutex);
        ++_inboundGeneration;
        // 线程已 join，无并发写者：同样整体赋值归还峰值容量。
        _inbound = QByteArray{};
        _inboundHead = 0;
        _inboundClosed = false;
        _inboundScheduled = false;
    }
    {
        QMutexLocker lock(&_writeMutex);
        _writeQueue = QByteArray{};  // 同上：归还 1 MiB 峰值容量
        _pendingWriteBytes.store(0, std::memory_order_release);
    }

    // 主动断开使旧的输入/EOF 投递失效，必须在清理完成后自行通知一次。
    // 同步发布可让 Session 及时进入 Failed，且不会迟到下一代连接。
    if (wasConnected)
        emit disconnected();
}

void SshTransport::write(const QByteArray& data)
{
    if (data.isEmpty() || !_running.load(std::memory_order_acquire))
        return;

    QMutexLocker lock(&_writeMutex);
    if (_pendingWriteBytes.load(std::memory_order_relaxed) + data.size()
        > MaxPendingWriteBytes) {
        reportError(tr("SSH write queue exceeded its 1 MiB limit."),
                    TransportErrorCategory::Overload);
        return;
    }
    _writeQueue.append(data);
    _pendingWriteBytes.fetch_add(data.size(), std::memory_order_release);
    _wakeup->notify();
}

void SshTransport::resizeTerminal(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    _pendingCols.store(cols);
    _pendingRows.store(rows);
    _wakeup->notify();
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
    if (!paused) {
        QMutexLocker lock(&_inboundMutex);
        scheduleInboundLocked();
    }
    _wakeup->notify();
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

bool SshTransport::executeCommand(quint64 requestId, QByteArray command)
{
    if (!_connected.load(std::memory_order_acquire) || requestId == 0
        || command.isEmpty() || command.size() > MaxCommandBytes) {
        return false;
    }

    // 此方法由 GUI 线程提交，工作线程在事件循环中取走；只保留一个待执行请求。
    QMutexLocker lock(&_commandMutex);
    if (_commandActive.load(std::memory_order_acquire)
        || !_commandQueue.isEmpty())
        return false;
    _commandQueue.enqueue(CommandRequest{requestId, std::move(command), {}, 0, false});
    _wakeup->notify();
    return true;
}

bool SshTransport::executeBoundedCommand(quint64 requestId, QByteArray command,
                                         CommandExecutionLimits limits)
{
    if (!_connected.load(std::memory_order_acquire) || requestId == 0
        || command.isEmpty() || command.size() > MaxCommandBytes
        || command.contains('\0') || limits.maxOutputBytes <= 0
        || limits.maxOutputBytes > MaxCommandOutputBytes
        || limits.timeoutMs <= 0 || limits.timeoutMs > CommandTimeoutMs)
        return false;
    QMutexLocker lock(&_commandMutex);
    if (_commandActive.load(std::memory_order_acquire) || !_commandQueue.isEmpty())
        return false;
    _commandQueue.enqueue(CommandRequest{requestId, std::move(command), limits,
        connectionGeneration(), true});
    _wakeup->notify();
    return true;
}

QString SshTransport::serverHostKeyFingerprint() const
{
    QMutexLocker lock(&_keyMutex);
    return _serverHostKeyFingerprint;
}

void SshTransport::cancelCommand(quint64 requestId)
{
    if (requestId == 0)
        return;
    QMutexLocker lock(&_commandMutex);
    if (!_commandQueue.isEmpty()
        && _commandQueue.head().requestId == requestId) {
        const auto request = _commandQueue.dequeue();
        if (request.bounded) {
            CommandExecutionResult result;
            result.requestId = requestId;
            result.connectionGeneration = request.generation;
            result.outcome = CommandExecutionOutcome::Cancelled;
            emitBoundedCommandFinished(std::move(result));
        }
        emitCommandFinished(requestId, {}, {}, tr("Remote command cancelled."));
        return;
    }
    _cancelCommandRequestId = requestId;
    _wakeup->notify();
}

void SshTransport::startResourceMonitoring()
{
    if (!_connected.load(std::memory_order_acquire))
        return;
    QMutexLocker lock(&_monitorMutex);
    if (_monitorEnabled)
        return;
    _monitorEnabled = true;
    _wakeup->notify();
    ++_monitorGeneration;
    _monitorRequestId = 0;
}

void SshTransport::stopResourceMonitoring()
{
    QMutexLocker lock(&_monitorMutex);
    if (!_monitorEnabled && _monitorRequestId == 0)
        return;
    _monitorEnabled = false;
    ++_monitorGeneration;
    _monitorRequestId = 0;
    _wakeup->notify();
}

bool SshTransport::requestResourceSample(quint64 requestId)
{
    if (!_connected.load(std::memory_order_acquire) || requestId == 0)
        return false;
    QMutexLocker lock(&_monitorMutex);
    if (!_monitorEnabled || _monitorRequestId != 0)
        return false;
    _monitorRequestId = requestId;
    _wakeup->notify();
    return true;
}

void SshTransport::reportError(const QString& message,
                               TransportErrorCategory category, bool retryable)
{
    {
        QMutexLocker lock(&_errorMutex);
        _errorString = message;
    }
    // 两条信号在同一次 invokeMethod 内按约定顺序发出，保证 TerminalSession
    // 看到 errorOccurred 时 transportError 已经到达。
    QMetaObject::invokeMethod(this, [this, message, category, retryable]() {
        emit transportError(TransportError{category, 0, message, retryable});
        emit errorOccurred(message);
    }, Qt::QueuedConnection);
}

void SshTransport::emitReadyRead(const QByteArray& data)
{
    QMutexLocker lock(&_inboundMutex);
    // worker 读取前按剩余容量限长，唯一消费者只会增加可用容量。
    Q_ASSERT(data.size() <= MaxInboundBytes - (_inbound.size() - _inboundHead));
    if (_inboundHead > 0 && _inbound.size() + data.size() > MaxInboundBytes) {
        _inbound.remove(0, _inboundHead);
        _inboundHead = 0;
    }
    _inbound.append(data);
    _inboundReceivedBytes += static_cast<quint64>(data.size());
    _inboundPeakBytes = std::max(_inboundPeakBytes, _inbound.size() - _inboundHead);
    scheduleInboundLocked();
}

qsizetype SshTransport::inboundCapacity() const
{
    QMutexLocker lock(&_inboundMutex);
    return MaxInboundBytes - (_inbound.size() - _inboundHead);
}

SshTransport::InboundStatistics SshTransport::inboundStatistics() const
{
    QMutexLocker lock(&_inboundMutex);
    return {_inboundReceivedBytes, _inboundDeliveredBytes,
            _inbound.size() - _inboundHead, _inboundPeakBytes};
}

void SshTransport::scheduleInboundLocked()
{
    if (_inboundScheduled || _readPaused.load(std::memory_order_acquire)
        || (_inbound.size() == _inboundHead && !_inboundClosed))
        return;
    _inboundScheduled = true;
    const quint64 generation = _inboundGeneration;
    QMetaObject::invokeMethod(this, [this, generation] {
        deliverInbound(generation);
    }, Qt::QueuedConnection);
}

void SshTransport::deliverInbound(quint64 generation)
{
    QByteArray bytes;
    {
        QMutexLocker lock(&_inboundMutex);
        if (generation != _inboundGeneration)
            return;
        if (_readPaused.load(std::memory_order_acquire)) {
            _inboundScheduled = false;
            return;
        }
        const qsizetype count = std::min(InboundDeliveryBytes,
                                         _inbound.size() - _inboundHead);
        bytes = _inbound.mid(_inboundHead, count);
        _inboundHead += count;
        _inboundDeliveredBytes += static_cast<quint64>(count);
        if (_inboundHead == _inbound.size()) {
            _inbound.clear();
            _inboundHead = 0;
        }
    }
    // 不持锁发信号：输入泵可同步暂停，甚至关闭/重连当前 transport。
    QPointer<SshTransport> guard(this);
    if (!bytes.isEmpty())
        emit readyRead(bytes);
    if (!guard)
        return;
    _wakeup->notify();
    bool closed = false;
    {
        QMutexLocker lock(&_inboundMutex);
        if (generation != _inboundGeneration)
            return;
        _inboundScheduled = false;
        closed = _inboundClosed && _inbound.isEmpty();
        if (closed)
            _inboundClosed = false;
        else
            scheduleInboundLocked();
    }
    if (closed && _connected.exchange(false))
        emit disconnected();
}

void SshTransport::emitSignal(void (SshTransport::*signal)())
{
    QMetaObject::invokeMethod(this, [this, signal]() {
        emit (this->*signal)();
    }, Qt::QueuedConnection);
}

void SshTransport::emitCommandFinished(quint64 requestId,
                                       QByteArray standardOutput,
                                       QByteArray standardError,
                                       QString errorMessage)
{
    // 移动捕获可避免复制较大的输出，同时保证信号最终在对象所属的 GUI 线程发出。
    QMetaObject::invokeMethod(
        this,
        [this, requestId, standardOutput = std::move(standardOutput),
         standardError = std::move(standardError),
         errorMessage = std::move(errorMessage)]() {
            emit commandFinished(requestId, standardOutput, standardError,
                                 errorMessage);
        },
        Qt::QueuedConnection);
}

void SshTransport::emitBoundedCommandFinished(CommandExecutionResult result)
{
    QMetaObject::invokeMethod(this, [this, result = std::move(result)] {
        emit boundedCommandFinished(result);
    }, Qt::QueuedConnection);
}

void SshTransport::emitResourceSampleFinished(quint64 requestId,
                                              QByteArray payload,
                                              QString errorMessage)
{
    QMetaObject::invokeMethod(
        this,
        [this, requestId, payload = std::move(payload),
         errorMessage = std::move(errorMessage)]() {
            emit resourceSampleFinished(requestId, payload, errorMessage);
        },
        Qt::QueuedConnection);
}

void SshTransport::workerMain()
{
    NovaTerm::setCurrentThreadName(SshWorkerThreadName);
    ssh_session session = ssh_new();
    if (!session) {
        reportError(tr("Failed to create SSH session."),
                    TransportErrorCategory::Unknown);
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
        QDir::toNativeSeparators(_knownHostsPath).toUtf8();
    const char* hostKeyAlgorithms =
        "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa";

    ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, knownHosts.constData());
    if (!_processUserConfiguration) {
        const auto directory = QDir::toNativeSeparators(QFileInfo(_knownHostsPath).absolutePath()).toUtf8();
        if (ssh_options_set(session, SSH_OPTIONS_PROCESS_CONFIG, &_processUserConfiguration) != SSH_OK
            || ssh_options_set(session, SSH_OPTIONS_SSH_DIR, directory.constData()) != SSH_OK
            || ssh_options_set(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, knownHosts.constData()) != SSH_OK) {
            reportError(QStringLiteral("Cannot isolate SSH configuration."), TransportErrorCategory::Configuration);
            ssh_free(session);
            return;
        }
    }
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSec);
    ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, hostKeyAlgorithms);

    // 显式应用选项以保留底层具体错误，避免 ssh_connect() 折叠配置失败原因。
    if (ssh_options_apply(session) != SSH_OK) {
        reportError(tr("Cannot apply SSH options: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::Configuration);
        ssh_free(session);
        return;
    }

    // ── 连接 ─────────────────────────────────────────────
    if (ssh_connect(session) != SSH_OK) {
        reportError(tr("SSH connection to %1:%2 failed: %3")
                        .arg(_config.host)
                        .arg(_config.port)
                        .arg(QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::Connection, true);
        ssh_free(session);
        return;
    }

    // ── 主机密钥验证（必须经过 UI 决策，绝不静默接受）──
    ssh_key serverKey = nullptr;
    if (ssh_get_server_publickey(session, &serverKey) != SSH_OK) {
        reportError(tr("Failed to retrieve the server host key: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::HostKey);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    const enum ssh_known_hosts_e knownState = ssh_session_is_known_server(session);
    if (knownState == SSH_KNOWN_HOSTS_ERROR) {
        reportError(tr("Cannot read known_hosts file %1: %2")
                        .arg(QString::fromUtf8(knownHosts),
                             QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::HostKey);
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
            reportError(tr("Host key verification failed; connection aborted."),
                    TransportErrorCategory::HostKey);
            ssh_key_free(serverKey);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }

        // 信任并写入 known_hosts（New 追加，Changed 更新）。
        if (ssh_session_update_known_hosts(session) != SSH_OK) {
            reportError(tr("Failed to store the host key: %1")
                            .arg(QString::fromUtf8(ssh_get_error(session))),
                        TransportErrorCategory::HostKey);
            ssh_key_free(serverKey);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
    }
    {
        unsigned char* hash = nullptr;
        size_t length = 0;
        QString fingerprint;
        if (ssh_get_publickey_hash(serverKey, SSH_PUBLICKEY_HASH_SHA256,
                                  &hash, &length) == SSH_OK && hash) {
            fingerprint = QString::fromLatin1(QByteArray(
                reinterpret_cast<const char*>(hash), qsizetype(length)).toHex());
            ssh_clean_pubkey_hash(&hash);
        }
        QMutexLocker lock(&_keyMutex);
        _serverHostKeyFingerprint = fingerprint;
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
                                 QString::fromUtf8(ssh_get_error(session))),
                        TransportErrorCategory::Configuration);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
        authResult = ssh_userauth_publickey(session, user.constData(), privkey);
        ssh_key_free(privkey);
        if (authResult != SSH_AUTH_SUCCESS) {
            reportError(tr("Public key authentication failed for %1@%2: %3")
                            .arg(_config.username, _config.host,
                                 QString::fromUtf8(ssh_get_error(session))),
                        TransportErrorCategory::Authentication);
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
                                 QString::fromUtf8(ssh_get_error(session))),
                        TransportErrorCategory::Authentication);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }
    }

    // ── 打开 channel：PTY + shell ────────────────────────
    ssh_channel channel = ssh_channel_new(session);
    if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
        reportError(tr("Failed to open SSH channel: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::Protocol);
        if (channel)
            ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    // 仅请求创建快照中明确携带的环境变量。服务器可拒绝 AcceptEnv；
    // 拒绝时 Shell 不会发布提示符标记，交互命令能力保持不可用。
    for (auto it = _config.environment.cbegin();
         it != _config.environment.cend(); ++it) {
        const QByteArray name = it.key().toUtf8();
        const QByteArray value = it.value().toUtf8();
        (void)ssh_channel_request_env(channel, name.constData(),
                                      value.constData());
    }

    const int startCols = _pendingCols.load() > 0 ? _pendingCols.load() : 80;
    const int startRows = _pendingRows.load() > 0 ? _pendingRows.load() : 24;

    if (ssh_channel_request_pty_size(channel, term.constData(),
                                     startCols, startRows) != SSH_OK
        || ssh_channel_request_shell(channel) != SSH_OK) {
        reportError(tr("Failed to start remote shell: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::Protocol);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    ssh_event event = ssh_event_new();
    if (!event) {
        reportError(tr("Failed to create SSH event loop."),
                    TransportErrorCategory::Unknown);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }
    ssh_event_add_session(event, session);
    if (ssh_event_add_fd(event, _wakeup->descriptor(), POLLIN,
            [](socket_t, int, void* context) {
                static_cast<SshWorkerWakeup*>(context)->consume();
                return 0;
            }, _wakeup.get()) != SSH_OK) {
        reportError(QStringLiteral("Cannot register SSH worker wakeup socket."),
                    TransportErrorCategory::Io);
        ssh_event_free(event);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    _connected.store(true);
    emitSignal(&SshTransport::connected);

    // 认证和主 Shell 建立完成后切换为非阻塞模式。此后所有可能返回
    // SSH_AGAIN 的 channel 操作由网络就绪或控制唤醒继续推进，避免辅助
    // channel 的 open/exec/read/write 阻塞交互终端。
    ssh_set_blocking(session, 0);

    // ── 事件循环：IO / resize / keepalive / 断线检测 ─────
    QElapsedTimer keepaliveTimer;
    keepaliveTimer.start();
    int appliedCols = startCols;
    int appliedRows = startRows;

    enum class ExecState { Closed, Opening, Starting, Running };

    // 通用单次命令与交互 Shell 复用 session，但使用独立 exec channel。
    ssh_channel commandChannel = nullptr;
    ExecState commandState{ExecState::Closed};
    QByteArray commandText;
    quint64 commandRequestId = 0;
    QByteArray commandOutput;
    QByteArray commandErrorOutput;
    QElapsedTimer commandTimer;
    CommandRequest activeCommand;
    bool commandMayHaveStarted = false;
    SshCommandCompletion commandCompletion;
    ssh_channel_callbacks_struct commandCallbacks{};
    commandCallbacks.userdata = &commandCompletion;
    commandCallbacks.channel_exit_status_function = commandExitStatusCallback;
    ssh_callbacks_init(&commandCallbacks);

    const auto finishCommand = [this, &commandChannel, &commandRequestId,
                                &commandOutput, &commandErrorOutput,
                                &commandState, &commandText,
                                &commandCompletion, &activeCommand,
                                &commandMayHaveStarted](
                                   QString errorMessage,
                                   CommandExecutionOutcome outcome = CommandExecutionOutcome::Failed) {
        if (!commandChannel)
            return;
        CommandExecutionResult result;
        result.requestId = commandRequestId;
        result.connectionGeneration = activeCommand.generation;
        result.outcome = errorMessage.isEmpty() ? CommandExecutionOutcome::Completed : outcome;
        result.executionMayHaveStarted = commandMayHaveStarted;
        result.terminationConfirmed = commandCompletion.result()
            == SshCommandCompletion::Result::Exited;
        result.exitCode = commandCompletion.exitStatus();
        result.outputTruncated = outcome == CommandExecutionOutcome::OutputLimit;
        if (activeCommand.bounded) {
            result.standardOutput = commandOutput;
            result.standardError = commandErrorOutput;
            emitBoundedCommandFinished(std::move(result));
        }
        if (!errorMessage.isEmpty())
            (void)ssh_channel_request_send_signal(commandChannel, "TERM");
        (void)ssh_channel_send_eof(commandChannel);
        ssh_channel_close(commandChannel);
        ssh_channel_free(commandChannel);
        commandChannel = nullptr;
        _commandActive.store(false, std::memory_order_release);
        commandState = ExecState::Closed;
        commandText.clear();
        commandCompletion.reset();
        emitCommandFinished(commandRequestId, std::move(commandOutput),
                            std::move(commandErrorOutput),
                            std::move(errorMessage));
        commandRequestId = 0;
        commandOutput.clear();
        commandErrorOutput.clear();
    };

    // 快速指标使用一个请求驱动的常驻 exec channel。远端脚本空闲时阻塞在
    // Shell 内建 read，不采样、不启动外部进程。
    ssh_channel monitorChannel = nullptr;
    ExecState monitorState{ExecState::Closed};
    quint64 monitorGeneration = 0;
    quint64 monitorRequestId = 0;
    QByteArray monitorWrite;
    QByteArray monitorStderr;
    SshMonitorFrameParser monitorParser;
    QElapsedTimer monitorTimer;
    QElapsedTimer workerTimer;
    workerTimer.start();
    qint64 monitorNextRetryMs = 0;
    int monitorFailureCount = 0;

    const auto closeMonitor = [&]() {
        if (monitorChannel) {
            // 正常暂停先送 EOF，使 read 退出；异常路径再尽力发送 TERM。所有调用
            // 都是非阻塞的，远端处于不可中断 I/O 时不等待其立即退出。
            (void)ssh_channel_send_eof(monitorChannel);
            (void)ssh_channel_close(monitorChannel);
            ssh_channel_free(monitorChannel);
        }
        monitorChannel = nullptr;
        monitorState = ExecState::Closed;
        monitorRequestId = 0;
        monitorWrite.clear();
        monitorStderr.clear();
        monitorParser.reset();
    };

    const auto finishMonitorRequest =
        [this, &monitorGeneration, &monitorRequestId](QByteArray payload,
                                                      QString error) {
        const quint64 completedId = monitorRequestId;
        bool deliver = false;
        {
            QMutexLocker lock(&_monitorMutex);
            if (_monitorEnabled
                && _monitorGeneration == monitorGeneration
                && _monitorRequestId == completedId) {
                _monitorRequestId = 0;
                deliver = true;
            }
        }
        monitorRequestId = 0;
        if (deliver && completedId != 0) {
            emitResourceSampleFinished(completedId, std::move(payload),
                                       std::move(error));
        }
    };

    const auto failMonitor = [&](const QString& error) {
        if (monitorChannel)
            (void)ssh_channel_request_send_signal(monitorChannel, "TERM");
        finishMonitorRequest({}, error);
        closeMonitor();
        monitorFailureCount = (std::min)(monitorFailureCount + 1, 6);
        const int shift = (std::min)(monitorFailureCount - 1, 5);
        const int backoff = (std::min)(1000 * (1 << shift),
                                       MonitorMaxBackoffMs);
        monitorNextRetryMs = workerTimer.elapsed() + backoff;
    };

    // 注意：tr() 是静态成员，lambda 内不需要捕获 this。
    const auto monitorProtocolError = [](
        SshMonitorFrameParser::Error error) {
        using Error = SshMonitorFrameParser::Error;
        switch (error) {
        case Error::BufferLimit:
            return tr("Resource monitor receive buffer exceeded 256 KiB.");
        case Error::LineLimit:
            return tr("Resource monitor line exceeded 16 KiB.");
        case Error::InvalidBegin:
            return tr("Resource monitor returned an invalid frame header.");
        case Error::MissingBegin:
            return tr("Resource monitor frame header was missing.");
        case Error::NestedBegin:
            return tr("Resource monitor returned overlapping frames.");
        case Error::MismatchedEnd:
            return tr("Resource monitor frame identifiers did not match.");
        case Error::EntryLimit:
            return tr("Resource monitor frame exceeded 256 entries.");
        case Error::FrameLimit:
            return tr("Resource monitor frame exceeded 128 KiB.");
        case Error::None:
        default:
            return QString{};
        }
    };

    QByteArray shellPendingWrite;
    qsizetype shellWriteHead = 0;
    int pollTimeoutMs = 0;

    while (_running.load(std::memory_order_acquire)
           && channel
           && ssh_is_connected(session)) {
        if (ssh_event_dopoll(event, pollTimeoutMs) == SSH_ERROR) {
            reportError(QStringLiteral("SSH event loop failed."),
                        TransportErrorCategory::Io, true);
            _running.store(false);
        }
        if (!_running.load(std::memory_order_acquire))
            break;
        pollTimeoutMs = -1;
        const auto deadline = [&pollTimeoutMs](qint64 remaining) {
            const int bounded = static_cast<int>(std::clamp<qint64>(remaining, 0, INT_MAX));
            pollTimeoutMs = pollTimeoutMs < 0 ? bounded : std::min(pollTimeoutMs, bounded);
        };

        // drain 对端数据（暂停时仍轮询协议，避免 SSH 窗口/流控死锁）
        if (!_readPaused.load(std::memory_order_acquire)) {
            QElapsedTimer readTimer;
            readTimer.start();
            qsizetype readBytes = 0;
            while (readBytes < ShellReadBudgetBytes && readTimer.elapsed() < ReadBudgetMs
                   && _running.load(std::memory_order_acquire)) {
                char buf[65536];
                const qsizetype available = inboundCapacity();
                if (available == 0 || _readPaused.load(std::memory_order_acquire))
                    break;
                const int n =
                    ssh_channel_read_nonblocking(channel, buf,
                        static_cast<uint32_t>(std::min<qsizetype>(sizeof(buf), available)), 0);
                if (n > 0) {
                    readBytes += n;
                    emitReadyRead(QByteArray(buf, n));
                } else if (n == 0 || n == SSH_AGAIN) {
                    break;   // 当前无数据
                } else if (ssh_channel_is_eof(channel)) {
                    _running.store(false);
                    break;
                } else {
                    reportError(tr("SSH channel read error: %1")
                                    .arg(QString::fromUtf8(
                                        ssh_get_error(session))),
                    TransportErrorCategory::Io, true);
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
        if (shellWriteHead > 0
            && shellPendingWrite.size() + toWrite.size() > MaxPendingWriteBytes) {
            shellPendingWrite.remove(0, shellWriteHead);
            shellWriteHead = 0;
        }
        shellPendingWrite.append(toWrite);
        if (!shellPendingWrite.isEmpty()) {
            const int writeSize = static_cast<int>((std::min)(
                shellPendingWrite.size() - shellWriteHead, qsizetype{64 * 1024}));
            const int n = ssh_channel_write(
                channel, shellPendingWrite.constData() + shellWriteHead,
                static_cast<uint32_t>(writeSize));
            if (n > 0) {
                shellWriteHead += n;
                if (shellWriteHead == shellPendingWrite.size()) {
                    shellPendingWrite.clear();
                    shellWriteHead = 0;
                }
                _pendingWriteBytes.fetch_sub(n, std::memory_order_release);
                if (!shellPendingWrite.isEmpty())
                    deadline(0);
                QMetaObject::invokeMethod(
                    this, [this, n] { emit bytesWritten(n); },
                    Qt::QueuedConnection);
            } else if (n != SSH_AGAIN) {
                    reportError(tr("SSH channel write failed: %1")
                                    .arg(QString::fromUtf8(
                                        ssh_get_error(session))),
                    TransportErrorCategory::Io, true);
                _running.store(false);
            }
        }

        // 通用命令以非阻塞状态机推进。df 即使响应慢，循环仍会继续处理主 Shell
        // 和常驻快速采样通道。
        if (commandChannel) {
            bool cancelled = false;
            {
                QMutexLocker lock(&_commandMutex);
                if (_cancelCommandRequestId == commandRequestId) {
                    _cancelCommandRequestId = 0;
                    cancelled = true;
                }
            }
            if (cancelled)
                finishCommand(tr("Remote command cancelled."), CommandExecutionOutcome::Cancelled);
        }
        if (!commandChannel) {
            CommandRequest request;
            bool hasRequest = false;
            {
                QMutexLocker lock(&_commandMutex);
                if (!_commandQueue.isEmpty()) {
                    request = _commandQueue.dequeue();
                    _commandActive.store(true, std::memory_order_release);
                    hasRequest = true;
                }
            }

            if (hasRequest) {
                commandChannel = ssh_channel_new(session);
                _commandActive.store(true, std::memory_order_release);
                commandRequestId = request.requestId;
                commandText = std::move(request.command);
                activeCommand = request;
                commandMayHaveStarted = false;
                commandCompletion.reset();
                if (!commandChannel) {
                    const QString error = tr("Failed to execute remote command: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session)));
                    emitCommandFinished(commandRequestId, {}, {}, error);
                    if (activeCommand.bounded) {
                        CommandExecutionResult result;
                        result.requestId = commandRequestId;
                        result.connectionGeneration = activeCommand.generation;
                        emitBoundedCommandFinished(std::move(result));
                    }
                    commandRequestId = 0;
                    _commandActive.store(false, std::memory_order_release);
                } else if (ssh_set_channel_callbacks(
                               commandChannel, &commandCallbacks) != SSH_OK) {
                    finishCommand(tr("Failed to monitor remote command completion: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
                } else {
                    commandState = ExecState::Opening;
                    commandTimer.restart();
                }
            }
        }

        if (commandChannel) {
            if (commandState == ExecState::Opening) {
                const int rc = ssh_channel_open_session(commandChannel);
                if (rc == SSH_OK)
                    commandState = ExecState::Starting;
                else if (rc != SSH_AGAIN)
                    finishCommand(tr("Failed to open remote command channel: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
            }
            if (commandChannel && commandState == ExecState::Starting) {
                // SSH_AGAIN 也可能已将 exec 请求送出，保守记录，不能声称未执行。
                commandMayHaveStarted = true;
                const int rc = ssh_channel_request_exec(
                    commandChannel, commandText.constData());
                if (rc == SSH_OK)
                    commandState = ExecState::Running;
                else if (rc != SSH_AGAIN)
                    finishCommand(tr("Failed to execute remote command: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
            }

            bool outputLimitExceeded = false;
            bool commandReadFailed = false;
            // 非阻塞读取 stdout/stderr，不能让监控命令拖住交互 Shell 的事件循环。
            const auto drainCommandStream = [&](int stream,
                                                QByteArray& destination) {
                qsizetype readBytes = 0;
                QElapsedTimer readTimer;
                readTimer.start();
                while (readBytes < AuxiliaryReadBudgetBytes && readTimer.elapsed() < ReadBudgetMs
                       && _running.load(std::memory_order_acquire)) {
                    char buffer[16 * 1024];
                    const qsizetype remaining = activeCommand.bounded
                        ? activeCommand.limits.maxOutputBytes
                            - commandOutput.size() - commandErrorOutput.size()
                        : MaxCommandOutputBytes - destination.size();
                    const auto readSize = activeCommand.bounded
                        ? std::min(qsizetype(sizeof(buffer)), remaining + 1) : qsizetype(sizeof(buffer));
                    const int count = ssh_channel_read_nonblocking(
                        commandChannel, buffer, uint32_t(readSize), stream);
                    if (count == 0 || count == SSH_AGAIN
                        || count == SSH_EOF) {
                        break;
                    }
                    if (count == SSH_ERROR) {
                        commandReadFailed = true;
                        break;
                    }
                    if (count > remaining) {
                        if (activeCommand.bounded && remaining > 0)
                            destination.append(buffer, remaining);
                        outputLimitExceeded = true;
                        break;
                    }
                    destination.append(buffer, count);
                    readBytes += count;
                }
            };
            if (commandChannel && commandState == ExecState::Running) {
                drainCommandStream(0, commandOutput);
                if (!outputLimitExceeded && !commandReadFailed)
                    drainCommandStream(1, commandErrorOutput);
                if ((ssh_channel_is_eof(commandChannel)
                     || ssh_channel_is_closed(commandChannel))
                    && ssh_channel_poll(commandChannel, 0) <= 0
                    && ssh_channel_poll(commandChannel, 1) <= 0) {
                    commandCompletion.observeOutputEnd();
                }
                if (ssh_channel_is_closed(commandChannel)
                    && ssh_channel_poll(commandChannel, 0) <= 0
                    && ssh_channel_poll(commandChannel, 1) <= 0)
                    commandCompletion.observeRemoteClose();
            }

            const auto completionResult = commandCompletion.result();
            if (!commandChannel) {
                // 状态推进失败时 finishCommand 已完成清理。
            } else if (commandReadFailed) {
                finishCommand(tr("Remote command read failed: %1")
                    .arg(QString::fromUtf8(ssh_get_error(session))));
            } else if (outputLimitExceeded) {
                finishCommand(tr("Remote command output exceeded its limit."),
                              CommandExecutionOutcome::OutputLimit);
            } else if (completionResult
                       == SshCommandCompletion::Result::Exited) {
                const int exitStatus = *commandCompletion.exitStatus();
                finishCommand(exitStatus == 0
                    ? QString{}
                    : tr("Remote command exited with status %1.").arg(exitStatus));
            } else if (completionResult
                       == SshCommandCompletion::Result::MissingExitStatus) {
                finishCommand(tr("Remote command closed without an exit status."));
            } else if (commandTimer.elapsed() >= (activeCommand.bounded
                           ? activeCommand.limits.timeoutMs : CommandTimeoutMs)) {
                finishCommand(tr("Remote command timed out."), CommandExecutionOutcome::TimedOut);
            }
        }

        // 读取 GUI 控制面的快照。generation 变化意味着暂停、会话切换或重启；
        // 旧 channel 和旧请求结果均不得进入新的 UI 上下文。
        bool monitorEnabled = false;
        quint64 requestedMonitorId = 0;
        quint64 requestedGeneration = 0;
        {
            QMutexLocker lock(&_monitorMutex);
            monitorEnabled = _monitorEnabled;
            requestedMonitorId = _monitorRequestId;
            requestedGeneration = _monitorGeneration;
        }

        if ((!monitorEnabled || (monitorChannel
             && requestedGeneration != monitorGeneration))) {
            closeMonitor();
        }
        if (monitorEnabled && !monitorChannel
            && workerTimer.elapsed() >= monitorNextRetryMs) {
            monitorChannel = ssh_channel_new(session);
            if (!monitorChannel) {
                monitorGeneration = requestedGeneration;
                monitorRequestId = requestedMonitorId;
                failMonitor(tr("Failed to create the resource monitor channel: %1")
                    .arg(QString::fromUtf8(ssh_get_error(session))));
            } else {
                monitorGeneration = requestedGeneration;
                monitorState = ExecState::Opening;
                monitorTimer.restart();
            }
        }

        if (monitorChannel && monitorState == ExecState::Opening) {
            const int rc = ssh_channel_open_session(monitorChannel);
            if (rc == SSH_OK) {
                monitorState = ExecState::Starting;
            } else if (rc != SSH_AGAIN) {
                monitorRequestId = requestedMonitorId;
                failMonitor(tr("Failed to open the resource monitor channel: %1")
                    .arg(QString::fromUtf8(ssh_get_error(session))));
            }
        }
        if (monitorChannel && monitorState == ExecState::Starting) {
            const QByteArray command = resourceMonitorCommand();
            const int rc = ssh_channel_request_exec(monitorChannel,
                                                     command.constData());
            if (rc == SSH_OK) {
                monitorState = ExecState::Running;
                monitorFailureCount = 0;
                monitorNextRetryMs = 0;
            } else if (rc != SSH_AGAIN) {
                monitorRequestId = requestedMonitorId;
                failMonitor(tr("Failed to start the resource monitor: %1")
                    .arg(QString::fromUtf8(ssh_get_error(session))));
            }
        }
        if (monitorChannel
            && (monitorState == ExecState::Opening
                || monitorState == ExecState::Starting)
            && monitorTimer.elapsed() >= MonitorEstablishTimeoutMs) {
            monitorRequestId = requestedMonitorId;
            failMonitor(tr("Resource monitor channel setup timed out."));
        }

        if (monitorChannel && monitorState == ExecState::Running) {
            if (monitorRequestId == 0 && requestedMonitorId != 0
                && requestedGeneration == monitorGeneration) {
                monitorRequestId = requestedMonitorId;
                monitorWrite = QByteArray::number(monitorRequestId) + '\n';
                monitorTimer.restart();
            }
            if (!monitorWrite.isEmpty()) {
                const int n = ssh_channel_write(
                    monitorChannel, monitorWrite.constData(),
                    static_cast<uint32_t>(monitorWrite.size()));
                if (n > 0) {
                    monitorWrite.remove(0, n);
                    if (monitorWrite.isEmpty())
                        monitorTimer.restart();
                } else if (n != SSH_AGAIN) {
                    failMonitor(tr("Failed to write a resource sample request: %1")
                        .arg(QString::fromUtf8(ssh_get_error(session))));
                }
            }

            bool monitorReadFailed = false;
            for (int stream : {0, 1}) {
                qsizetype readBytes = 0;
                QElapsedTimer readTimer;
                readTimer.start();
                while (readBytes < AuxiliaryReadBudgetBytes && readTimer.elapsed() < ReadBudgetMs
                       && _running.load(std::memory_order_acquire)) {
                    char buffer[16 * 1024];
                    const int count = ssh_channel_read_nonblocking(
                        monitorChannel, buffer, sizeof(buffer), stream);
                    if (count == 0 || count == SSH_AGAIN
                        || count == SSH_EOF) {
                        break;
                    }
                    if (count < 0) {
                        monitorReadFailed = true;
                        break;
                    }
                    readBytes += count;
                    if (stream == 1) {
                        if (monitorStderr.size() + count
                            > MaxMonitorStderrBytes) {
                            monitorReadFailed = true;
                            break;
                        }
                        monitorStderr.append(buffer, count);
                        continue;
                    }

                    auto parsed = monitorParser.append(
                        QByteArray(buffer, count));
                    if (parsed.error != SshMonitorFrameParser::Error::None) {
                        failMonitor(monitorProtocolError(parsed.error));
                        break;
                    }
                    for (SshMonitorFrame& frame : parsed.frames) {
                        if (monitorRequestId == 0
                            || frame.requestId != monitorRequestId) {
                            failMonitor(tr("Resource monitor returned an unexpected frame."));
                            break;
                        }
                        finishMonitorRequest(std::move(frame.payload), {});
                        monitorStderr.clear();
                    }
                    if (!monitorChannel)
                        break;
                }
                if (!monitorChannel || monitorReadFailed)
                    break;
            }
            if (monitorChannel && monitorReadFailed) {
                failMonitor(tr("Resource monitor output was invalid or exceeded its limit."));
            } else if (monitorChannel && ssh_channel_is_eof(monitorChannel)) {
                const QString details = QString::fromUtf8(monitorStderr).trimmed();
                failMonitor(details.isEmpty()
                    ? tr("Resource monitor channel closed unexpectedly.")
                    : tr("Resource monitor failed: %1").arg(details));
            } else if (monitorChannel && monitorRequestId != 0
                       && monitorWrite.isEmpty()
                       && monitorTimer.elapsed() >= MonitorResponseTimeoutMs) {
                failMonitor(tr("Resource sample timed out."));
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
            const int resizeResult =
                ssh_channel_change_pty_size(channel, pc, pr);
            if (resizeResult != SSH_OK && resizeResult != SSH_AGAIN) {
                reportError(tr("Failed to resize the remote PTY: %1")
                                .arg(QString::fromUtf8(ssh_get_error(session))),
                    TransportErrorCategory::Protocol);
            }
            if (resizeResult == SSH_OK) {
                appliedCols = pc;
                appliedRows = pr;
                // 保留最新请求值，避免清空时覆盖 GUI 并发提交的新尺寸。
            }
        }

        // keepalive：发送 SSH_MSG_IGNORE 保活（0 表示禁用）
        if (_keepAliveMs > 0 && keepaliveTimer.elapsed() >= _keepAliveMs) {
            ssh_send_ignore(session, "");
            keepaliveTimer.restart();
        }

        if ((ssh_channel_is_eof(channel) && ssh_channel_poll(channel, 0) <= 0)
            || !ssh_is_connected(session))
            _running.store(false);

        if (_keepAliveMs > 0)
            deadline(_keepAliveMs - keepaliveTimer.elapsed());
        if (commandChannel)
            deadline((activeCommand.bounded ? activeCommand.limits.timeoutMs : CommandTimeoutMs)
                     - commandTimer.elapsed());
        if (monitorEnabled && !monitorChannel)
            deadline(monitorNextRetryMs - workerTimer.elapsed());
        if (monitorChannel && monitorState != ExecState::Running)
            deadline(MonitorEstablishTimeoutMs - monitorTimer.elapsed());
        if (monitorChannel && monitorRequestId != 0)
            deadline(MonitorResponseTimeoutMs - monitorTimer.elapsed());
        // libssh 中可能已有未消费字节，不能只等待下一次 socket 可读。
        if (!_readPaused.load(std::memory_order_acquire) && inboundCapacity() > 0
            && ssh_channel_poll(channel, 0) > 0)
            deadline(0);
        if (commandChannel && (ssh_channel_poll(commandChannel, 0) > 0
                               || ssh_channel_poll(commandChannel, 1) > 0))
            deadline(0);
        if (monitorChannel && (ssh_channel_poll(monitorChannel, 0) > 0
                               || ssh_channel_poll(monitorChannel, 1) > 0))
            deadline(0);
    }

    // ── 关闭 ─────────────────────────────────────────────
    const bool wasConnected = _connected.load();

    if (commandChannel)
        finishCommand(tr("SSH connection closed before the command completed."),
                      CommandExecutionOutcome::Disconnected);
    closeMonitor();

    if (channel) {
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
    }
    if (event) {
        ssh_event_remove_fd(event, _wakeup->descriptor());
        ssh_event_free(event);
    }
    if (ssh_is_connected(session))
        ssh_disconnect(session);
    ssh_free(session);

    if (wasConnected) {
        QMutexLocker lock(&_inboundMutex);
        _inboundClosed = true;
        scheduleInboundLocked();
    }
}
