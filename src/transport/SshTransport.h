#pragma once

#include "ITransport.h"
#include "session/SessionTypes.h"

#include <QAtomicInt>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>

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
    explicit SshTransport(SshConfig config, QObject* parent = nullptr);
    ~SshTransport() override;

    bool connectToHost() override;
    void disconnect() override;
    void write(const QByteArray& data) override;
    void resizeTerminal(int cols, int rows) override;
    [[nodiscard]] bool isConnected() const override;
    [[nodiscard]] QString errorString() const override;
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

signals:
    // 需要 UI 决策：首次信任 / 主机密钥变更。未处理（无连接）时等待方会
    // 因 disconnect 或超时安全中止，不会永久阻塞。
    void hostKeyRequired(const SshHostKeyInfo& info);

private:
    void workerMain();          // 在工作线程中运行整个会话生命周期
    void reportError(const QString& message);   // 线程安全：记录 + 投递信号
    void emitReadyRead(const QByteArray& data);
    void emitSignal(void (SshTransport::*signal)());

    static constexpr qint64 MaxPendingWriteBytes = 1024 * 1024;
    static constexpr int ConnectTimeoutSec = 10;
    static constexpr int TeardownWaitMs = 15000;

    SshConfig _config;
    int _keepAliveMs{0};

    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _readPaused{false};

    // 写队列：GUI 线程 append，工作线程在事件循环里 drain。
    mutable QMutex _writeMutex;
    QByteArray _writeQueue;

    // 待处理 PTY 尺寸：-1 表示无。
    std::atomic<int> _pendingCols{-1};
    std::atomic<int> _pendingRows{-1};

    // 主机密钥决策：-1 未决，0 拒绝，1 接受。
    mutable QMutex _keyMutex;
    int _keyDecision{-1};
    QWaitCondition _keyWait;

    mutable QMutex _errorMutex;
    QString _errorString;

    QThread* _thread{nullptr};
};
