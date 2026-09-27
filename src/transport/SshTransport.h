/**
 * @file   SshTransport.h
 * @brief  SSH 传输：基于 libssh 的 ITransport 实现。
 *
 * libssh 的连接与认证 API 是同步阻塞的，因此本类将整个会话生命周期放在一个
 * 专用工作线程中；认证完成后的 channel I/O 改为非阻塞状态机。GUI 线程通过
 * 原子量 / 互斥队列提交请求：
 *   GUI 线程 → 工作线程：connectToHost()/write()/resizeTerminal()/disconnect()
 *   工作线程 → GUI 线程：通过 QueuedConnection 投递信号（connected()/readyRead()等）
 *
 * 密码、私钥口令仅存在于 SshConfig 构造快照中，不写入日志或配置文件。
 */
#pragma once

#include "ITransport.h"
#include "session/SessionTypes.h"
#include "session/CommandExecutionTypes.h"

#include <QAtomicInt>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>

class SshWorkerWakeup;

// SshTransport — ITransport 实现，基于 libssh 的 SSH shell 会话。
//
// libssh 的会话 API 是同步阻塞的，因此本类把整个会话生命周期放在一个
// 专用工作线程里（QThread::create 的 lambda 线程，与 LocalShellTransport
// 的 reader 线程同一模式），GUI 线程只通过原子量 / 互斥队列提交请求：
//
//   GUI 线程                                   工作线程
//   ─────────                                  ─────────
//   connectToHost()   ── 启动线程 ─────────▶   ssh_connect → 主机密钥 → 认证
//   write()           ── 写队列 ───────────▶   事件循环(ssh_event_dopoll)
//   resizeTerminal()  ── 待处理尺寸 ───────▶   ssh_channel_request_pty_size
//   setReadPaused()   ── 原子标志 ─────────▶   暂停 drain 但继续协议轮询
//   accept/rejectHostKey() ── 条件量 ──────▶   唤醒主机密钥决策等待
//   disconnect()      ── 原子标志+唤醒 ────▶   清理并退出线程
//
// 从工作线程发出的信号一律通过 QMetaObject::invokeMethod(..., Qt::QueuedConnection)
// 投递到 GUI 线程（与 LocalShellTransport 相同），避免跨线程直连。
//
// 密码、私钥口令只存在于 SshConfig 构造快照中，绝不写入日志或配置文件。
class SshTransport final : public ITransport
{
    Q_OBJECT
public:
    struct InboundStatistics {
        quint64 receivedBytes{0};
        quint64 deliveredBytes{0};
        qsizetype pendingBytes{0};
        qsizetype peakBytes{0};
    };
    /** @brief 有界输入交付统计；在锁内取得一致副本。 */
    [[nodiscard]] InboundStatistics inboundStatistics() const;
    explicit SshTransport(SshConfig config, QObject* parent = nullptr);
    ~SshTransport() override;

    bool connectToHost() override;
    /**
     * @brief 回收当前连接；若原先已连接，清理结束时同步发出一次 disconnected。
     * @note 重复断开不重复通知，旧输入/EOF 投递在返回前失效。
     */
    void disconnect() override;
    void write(const QByteArray& data) override;
    void resizeTerminal(int cols, int rows) override;
    [[nodiscard]] bool isConnected() const override;
    /** @brief 连接代际；显式断开即递增，辅助数据缓存不可跨代复用。 */
    [[nodiscard]] quint64 connectionGeneration() const noexcept
    {
        return _connectionGeneration.load(std::memory_order_acquire);
    }
    [[nodiscard]] QString errorString() const override;
    /** SFTP 等同主机辅助通道使用的只读连接快照。 */
    [[nodiscard]] const SshConfig& sessionConfig() const noexcept
    {
        return _config;
    }
    /** @brief 当前 SSH 连接用于主机密钥信任的 known_hosts 路径。 */
    [[nodiscard]] const QString& knownHostsPath() const noexcept
    {
        return _knownHostsPath;
    }
    bool setReadPaused(bool paused) override;
    [[nodiscard]] TransportCapabilities capabilities() const override
    {
        return TransportCapability::PauseReads
            | TransportCapability::ResizeTerminal
            | TransportCapability::KeepAlive
            | TransportCapability::Reconnect;
    }

    // 主机密钥验证决策（GUI 线程调用，唤醒工作线程中被阻塞的等待）。
    void acceptHostKey();
    void rejectHostKey();

    /**
     * @brief 在当前 SSH 连接上异步执行一个非交互命令。
     * @param requestId 调用方生成的请求 ID，完成信号原样返回。
     * @param command   UTF-8 Shell 命令。
     * @return 已加入有界队列时返回 true；未连接、命令无效或已有请求时返回 false。
     */
    [[nodiscard]] bool executeCommand(quint64 requestId, QByteArray command);
    /** @brief 提交具有合计输出预算的命令；调用方负责命令策略，不等待完成。 */
    [[nodiscard]] bool executeBoundedCommand(quint64 requestId, QByteArray command,
                                              CommandExecutionLimits limits = {});
    /** @brief 已经验证的服务端主机密钥指纹，仅用于本机目标身份隔离。 */
    [[nodiscard]] QString serverHostKeyFingerprint() const;
    /** 取消尚未开始或正在运行的指定通用命令。 */
    void cancelCommand(quint64 requestId);

    /** @brief 启用请求驱动的常驻资源采集通道；不会立即执行采样。 */
    void startResourceMonitoring();
    /** @brief 停止采集并请求工作线程关闭远端脚本与通道。 */
    void stopResourceMonitoring();
    /**
     * @brief 在常驻通道上请求一帧 CPU、内存和网络计数。
     * @return 请求已接收时返回 true；未连接、未启用或已有在途请求时返回 false。
     */
    [[nodiscard]] bool requestResourceSample(quint64 requestId);

signals:
    // 需要 UI 决策：首次信任 / 主机密钥变更。未处理（无连接）时等待方会
    // 因 disconnect 或超时安全中止，不会永久阻塞。
    void hostKeyRequired(const SshHostKeyInfo& info);

    /** 辅助 exec channel 完成；信号始终投递到 GUI 线程。 */
    void commandFinished(quint64 requestId, const QByteArray& standardOutput,
                         const QByteArray& standardError,
                         const QString& errorMessage);
    /** @brief 有界命令的完成证据；关闭通道不等于确认远端进程结束。 */
    void boundedCommandFinished(const CommandExecutionResult& result);
    /**
     * @brief 常驻资源采集通道的一次请求完成；失败时 payload 为空。
     * @note payload 按 @@stat / @@meminfo / @@loadavg / @@uptime 分节，
     *       meminfo 节尾附带 /proc/net/dev 原文；所有计算由消费方在本地完成。
     */
    void resourceSampleFinished(quint64 requestId, const QByteArray& payload,
                                const QString& errorMessage);

private:
    void emitBoundedCommandFinished(CommandExecutionResult result);
    std::atomic<quint64> _connectionGeneration{0};
    friend class SshTransportTestAccess;
    bool _processUserConfiguration{true}; ///< 隔离测试可关闭环境中的 SSH 配置，生产默认不变
    void scheduleInboundLocked();
    void deliverInbound(quint64 generation);
    [[nodiscard]] qsizetype inboundCapacity() const;
    void workerMain();          // 在工作线程中运行整个会话生命周期
    // 线程安全：记录 + 投递信号。先发 transportError 再发 errorOccurred，
    // 二者 message 一致（顺序约定见 ITransport.h）。
    void reportError(const QString& message,
                     TransportErrorCategory category = TransportErrorCategory::Io,
                     bool retryable = false);
    void emitReadyRead(const QByteArray& data);
    void emitSignal(void (SshTransport::*signal)());
    void emitCommandFinished(quint64 requestId, QByteArray standardOutput,
                             QByteArray standardError, QString errorMessage);
    void emitResourceSampleFinished(quint64 requestId, QByteArray payload,
                                    QString errorMessage);

    struct CommandRequest
    {
        // ID 只用于把异步结果匹配回调用方，不参与 SSH 协议。
        quint64 requestId{0};
        QByteArray command;
        CommandExecutionLimits limits;
        quint64 generation{0};
        bool bounded{false};
    };

    // 命令长度、输出量和执行时间均设上限，避免异常服务端耗尽本地资源。
    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024;
    static constexpr qsizetype MaxInboundBytes = 1024 * 1024;
    static constexpr qsizetype InboundDeliveryBytes = 64 * 1024;
    static constexpr qsizetype ShellReadBudgetBytes = 256 * 1024;
    static constexpr qsizetype AuxiliaryReadBudgetBytes = 64 * 1024;
    static constexpr int ReadBudgetMs = 2;
    static constexpr qsizetype MaxCommandBytes = 16 * 1024;
    static constexpr qsizetype MaxCommandOutputBytes = 1024 * 1024;
    static constexpr int CommandTimeoutMs = 5000;
    static constexpr int MonitorEstablishTimeoutMs = 5000;
    static constexpr int MonitorResponseTimeoutMs = 5000;
    static constexpr int MonitorMaxBackoffMs = 30000;
    static constexpr qsizetype MaxMonitorStderrBytes = 16 * 1024;
    static constexpr int ConnectTimeoutSec = 10;
    static constexpr int TeardownWaitMs = 15000;
    // 析构路径的等待上限：连接/密钥/认证/channel 每步阻塞调用以
    // ConnectTimeoutSec 为单步上限，串行最坏可超过单次 TeardownWaitMs。
    static constexpr int TeardownDestructorWaitMs = 60000;

    SshConfig _config;
    QString _knownHostsPath;
    int _keepAliveMs{0};

    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _readPaused{false};

    // 最多一个 queued delivery；暂停时保留字节，关闭/重连使旧投递失效。
    mutable QMutex _inboundMutex;
    QByteArray _inbound;
    qsizetype _inboundHead{0};
    quint64 _inboundGeneration{0};
    bool _inboundScheduled{false};
    bool _inboundClosed{false};
    quint64 _inboundReceivedBytes{0};
    quint64 _inboundDeliveredBytes{0};
    qsizetype _inboundPeakBytes{0};

    // 写队列：GUI 线程 append，工作线程在事件循环里 drain。
    mutable QMutex _writeMutex;
    QByteArray _writeQueue;
    std::atomic<qint64> _pendingWriteBytes{0};

    // 资源监控等低频辅助命令最多保留一个，防止慢服务端积压轮询任务。
    mutable QMutex _commandMutex;
    QQueue<CommandRequest> _commandQueue;
    quint64 _cancelCommandRequestId{0};
    std::atomic<bool> _commandActive{false};

    // 常驻监控控制面：GUI 线程只写此状态，SSH channel 始终由工作线程拥有。
    mutable QMutex _monitorMutex;
    bool _monitorEnabled{false};
    quint64 _monitorGeneration{0};
    quint64 _monitorRequestId{0};

    // 待处理 PTY 尺寸：-1 表示无。
    std::atomic<int> _pendingCols{-1};
    std::atomic<int> _pendingRows{-1};

    // 主机密钥决策：-1 未决，0 拒绝，1 接受。
    mutable QMutex _keyMutex;
    int _keyDecision{-1};
    QString _serverHostKeyFingerprint;
    QWaitCondition _keyWait;

    mutable QMutex _errorMutex;
    QString _errorString;

    QThread* _thread{nullptr};
    // true 表示上一任工作线程超时未退出、被放弃（finished→deleteLater
    // 自毁），它可能仍在访问本对象成员；此时禁止启动新会话。
    // 仅 GUI 线程读写。
    bool _workerAbandoned{false};
    std::unique_ptr<SshWorkerWakeup> _wakeup;

    void disconnectInternal(int waitMs);
};
