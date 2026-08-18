/**
 * @file   PtySession.h
 * @brief  Linux PTY 会话：基于 posix_openpt + fork 的本地 shell。
 *
 * 事件驱动的 Linux 伪终端会话。原生描述符与子进程生命周期均保留在
 * QObject 线程上，输入入队为线程安全（供 LocalShellTransport 跨线程调用）。
 * 通过 QSocketNotifier 监听 master fd 的可读/可写，QTimer 轮询子进程退出。
 */
#pragma once

#include "session/LocalShellProfile.h"
#include "transport/ITransport.h"

#include <QByteArray>
#include <QObject>

#include <cstddef>
#include <deque>
#include <mutex>

class QSocketNotifier;
class QTimer;

namespace NovaTerm::Linux {

/**
 * @brief Linux PTY 本地 shell 会话。
 *
 * start() 通过 forkpty 启动子进程，master fd 设为非阻塞 + CLOEXEC。
 * 输出经 QSocketNotifier(Read) 触发 drainOutput()；输入入队后由
 * flushInput() 写入 master，EAGAIN 时启用 Write notifier。
 * requestClose() 先 SIGHUP，超时升级为 SIGTERM/SIGKILL。
 */
class PtySession final : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Closing, Closed };
    Q_ENUM(State)

    /**
     * @brief 构造 PTY 会话。
     * @param config  本地 shell 配置。
     * @param columns 初始列数。
     * @param rows    初始行数。
     * @param parent  父对象。
     */
    explicit PtySession(LocalShellConfig config, int columns, int rows,
                        QObject* parent = nullptr);
    ~PtySession() override;

    /**
     * @brief 入队用户输入（线程安全）。
     * @param data 待写入的字节。
     * @return true 表示已入队；会话未运行或队列超限返回 false。
     */
    [[nodiscard]] bool tryEnqueueInput(const QByteArray& data);

public slots:
    void start();                       ///< 打开 PTY 并 fork 子进程
    void resize(int columns, int rows); ///< 通过 TIOCSWINSZ 调整 PTY 尺寸
    void setReadPaused(bool paused);    ///< 暂停/恢复读取
    void requestClose();                ///< 请求关闭（SIGHUP→SIGTERM→SIGKILL）

signals:
    void started();                                      ///< 会话已启动
    void dataReady(const QByteArray& data);              ///< 收到子进程输出
    void errorOccurred(const QString& error);            ///< 发生错误
    void exited(quint32 exitCode, TransportExitReason reason); ///< 子进程退出
    void closed();                                       ///< 会话已完全关闭
    void stateChanged(NovaTerm::Linux::PtySession::State state); ///< 状态变更

private:
    static constexpr std::size_t InputCapacity = 4U * 1024U * 1024U;  ///< 输入队列上限 4 MiB
    static constexpr std::size_t ReadBufferSize = 64U * 1024U;        ///< 单次读取缓冲 64 KiB

    void transition(State state);          ///< 状态迁移并发信号
    void drainOutput();                     ///< 从 master fd 读取并投递输出
    void flushInput();                      ///< 将输入队列写入 master fd
    void checkChildExit();                  ///< 轮询子进程退出状态
    void closeDescriptors();                ///< 关闭 master fd 与 notifier
    void finalizeClose();                   ///< 完成关闭流程并释放会话资源
    void failStart(const QString& error);   ///< 启动失败收尾
    void finish(int status);                ///< 解析退出状态并发 exited
    void reportIoError(const QString& operation, int error); ///< 上报 I/O 错误

    LocalShellConfig _config;
    State _state{State::Idle};
    int _columns{80};
    int _rows{24};
    int _masterFd{-1};            ///< PTY master 端文件描述符
    qint64 _childPid{-1};        ///< 子进程 PID
    QSocketNotifier* _readNotifier{nullptr};
    QSocketNotifier* _writeNotifier{nullptr};
    QTimer* _exitTimer{nullptr};
    bool _readPaused{false};
    bool _userCloseRequested{false};
    bool _exitEmitted{false};
    int _closePolls{0};           ///< 关闭轮询计数（用于升级信号）

    std::mutex _inputMutex;
    std::deque<QByteArray> _inputQueue;
    std::size_t _inputBytes{0};
    qsizetype _inputOffset{0};    ///< 当前队首未写入偏移
    bool _acceptingInput{false};
};

} // namespace NovaTerm::Linux
