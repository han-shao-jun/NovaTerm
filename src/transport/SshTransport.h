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

#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <utility>

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
    // 在工作线程中运行整个会话生命周期。wakeup 由调用方（线程 lambda）
    // 按值捕获一份 shared_ptr 并传入，故本函数体内一律用它而不是 _wakeup：
    // 被放弃的 worker 可能在 ~SshTransport 之后才返回，届时 _wakeup 已失效。
    void workerMain(const std::shared_ptr<SshWorkerWakeup>& wakeup);
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

    /** @brief 把 (cols, rows) 打包成单字待处理尺寸；任一 <= 0 时返回 0。 */
    [[nodiscard]] static constexpr uint32_t packPendingSize(int cols, int rows)
    {
        if (cols <= 0 || rows <= 0)
            return 0;
        const uint32_t c = cols > 0xFFFF ? 0xFFFFu : uint32_t(cols);
        const uint32_t r = rows > 0xFFFF ? 0xFFFFu : uint32_t(rows);
        return (c << 16) | r;
    }

    /** @brief 拆回 (cols, rows)；0 返回 {0, 0}。 */
    [[nodiscard]] static constexpr std::pair<int, int> unpackPendingSize(uint32_t packed)
    {
        if (packed == 0)
            return {0, 0};
        return {int(packed >> 16), int(packed & 0xFFFFu)};
    }
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

    /**
     * @brief 待处理 PTY 尺寸，打包成单个原子字发布。
     *
     * 曾用两个独立的 std::atomic<int>，于是「列取自第 N 次 resize、行取自
     * 第 N+1 次」这种撕裂读是可能的：worker 会应用一个既非旧尺寸也非新尺寸
     * 的几何（典型症状是 80 列配 50 行），并把它记入 appliedCols/appliedRows
     * 当作已应用，从而不再自愈。单个 32 位字里高 16 位存列、低 16 位存行，
     * 发布即原子，两个字段不可能来自不同的 resize。
     *
     * 0 表示「无请求」（resizeTerminal 拒绝 <= 0，故任何已发布值都非 0）。
     * 列/行各自 16 位对终端尺寸绰绰有余；超过 65535 的值直接钳到 65535。
     */
    std::atomic<uint32_t> _pendingSize{0};

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
    //
    // ⚠ 放弃路径的保证边界（不要误读成"析构安全"）：
    //   已消除：_wakeup 是最后声明的成员，也就是析构时最先销毁的那个，而
    //   僵尸 worker 仍把它当 libssh ssh_event_add_fd 的回调上下文 —— 改由
    //   线程 lambda 按值捕获的 shared_ptr 持有。
    //   仍在窗口内：僵尸 worker 还会触碰 _config（含口令/密钥材料）与各
    //   mutex。这些都在对象里，析构后即失效。要彻底消除需要把整个传输
    //   状态改为共享所有权（worker 持 shared_ptr<State>），那是 workerMain
    //   的整体重构，不是一处补丁。触发条件是 waitMs 内停不下来
    //   （非阻塞 libssh 路径下只可能是事件循环本身卡死），不是常规路径。
    bool _workerAbandoned{false};
    // 唤醒 socket 由 worker lambda 按值捕获一份 shared_ptr，因此它比本对象
    // 活得久。这不是可有可无的：_wakeup 是**最后一个**声明的成员，也就是
    // ~SshTransport 时**最先**被销毁的那个，而被放弃的僵尸 worker 仍把它
    // 当作 libssh ssh_event_add_fd 的回调上下文（并在拆除路径调 descriptor()）。
    // 用 unique_ptr 时那是对已释放内存的读写，且 libssh 手里还留着野指针。
    std::shared_ptr<SshWorkerWakeup> _wakeup;

    void disconnectInternal(int waitMs);
};
