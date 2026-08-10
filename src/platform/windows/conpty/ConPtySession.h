/**
 * @file   ConPtySession.h
 * @brief  Windows ConPTY 会话：本地 shell 的伪控制台封装。
 *
 * 通过 CreatePseudoConsole 创建伪控制台，CreateProcessW 启动子进程并绑定
 * Job Object（kill-on-close）。读/写/进程等待分别运行在专用线程上，
 * 通过有界队列与 GUI 线程交互。关闭流程复杂：需有序回收 writer、进程、
 * 伪控制台、reader，避免队列满与 ConPTY 输出管道满互相死锁。
 */
#pragma once

#include "ConPtyApi.h"
#include "WinHandle.h"
#include "session/LocalShellProfile.h"
#include "transport/ITransport.h"

#include <QByteArray>
#include <QObject>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

namespace NovaTerm::Windows {

struct AsyncPseudoConsoleClose;

/**
 * @brief ConPTY 本地 shell 会话。
 *
 * 线程模型：reader/writer/processWait 三个工作线程 + 伪控制台关闭线程，
 * 通过互斥队列与原子标志与 GUI 线程通信。GUI 线程仅调用线程安全的
 * handoff 方法（tryEnqueueInput/acknowledgeOutput）与 public slots。
 */
class ConPtySession final : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Closing, Closed };
    Q_ENUM(State)

    /**
     * @brief 构造 ConPTY 会话。
     * @param config  本地 shell 配置。
     * @param columns 初始列数。
     * @param rows    初始行数。
     * @param parent  父对象。
     */
    explicit ConPtySession(LocalShellConfig config, int columns, int rows,
                           QObject* parent = nullptr);
    ~ConPtySession() override;

    // 这两个方法是线程安全的 GUI 侧 handoff 操作，仅访问受互斥量/原子量
    // 保护的状态，绝不执行原生 I/O，可在任意线程调用。
    /**
     * @brief 入队用户输入（线程安全）。
     * @param data 待写入的字节。
     * @return true 表示已入队；会话未运行或队列超限返回 false。
     */
    [[nodiscard]] bool tryEnqueueInput(const QByteArray& data);

    /**
     * @brief 确认上一批输出已被消费（线程安全）。
     * @note 由 LocalShellTransport 在 readyRead 投递后调用，解除背压。
     */
    void acknowledgeOutput();

public slots:
    void start();                       ///< 启动 ConPTY 与子进程（须在生命周期线程调用）
    void resize(int columns, int rows); ///< 调整伪控制台尺寸
    void setReadPaused(bool paused);    ///< 暂停/恢复读取
    void requestClose();                ///< 请求用户主动关闭
    void close();                       ///< 关闭流程入口

signals:
    void started();                                      ///< 会话已启动
    void dataReady(const QByteArray& data);              ///< 收到子进程输出
    void errorOccurred(const QString& error);            ///< 发生错误
    void exited(quint32 exitCode, TransportExitReason reason); ///< 子进程退出
    void closed();                                       ///< 会话已完全关闭
    void stateChanged(NovaTerm::Windows::ConPtySession::State state); ///< 状态变更

private:
    static constexpr std::size_t InputCapacity = 4U * 1024U * 1024U;       ///< 输入队列上限 4 MiB
    static constexpr std::size_t OutputCapacity = 8U * 1024U * 1024U;      ///< 输出队列上限 8 MiB
    static constexpr std::size_t ReadBufferSize = 64U * 1024U;            ///< 单次读取缓冲 64 KiB
    static constexpr std::size_t OutputDeliveryBatch = 256U * 1024U;      ///< 单批投递上限 256 KiB

    struct StartupResources;

    [[nodiscard]] bool createStartupResources(StartupResources& resources,
                                              QString& error);  ///< 创建管道与伪控制台
    [[nodiscard]] bool launchProcess(StartupResources& resources,
                                     QString& error);          ///< 启动子进程并绑定 Job
    void readerMain();                    ///< 读取线程：从输出管道 drain
    void writerMain();                     ///< 写入线程：消费输入队列写入管道
    void processWaitMain();               ///< 进程等待线程：等待子进程退出
    void scheduleOutputDrain();            ///< 调度一次输出 drain 到 GUI 线程
    void drainOutput();                    ///< 在 GUI 线程批量投递输出
    void handleProcessExit(quint32 exitCode);     ///< 处理子进程退出
    void handleIoFailure(const QString& operation, unsigned long error); ///< 上报 I/O 失败并关闭
    void handlePipeTermination(const QString& operation, unsigned long error); ///< 管道对端关闭处理
    void confirmPipeTermination(const QString& operation, unsigned long error); ///< 延迟确认管道关闭
    void tryStartPseudoConsoleCloser();    ///< 启动专用线程关闭伪控制台
    void pollWriterClose();                ///< 轮询 writer 线程退出
    void continueCloseAfterWriter();       ///< writer 回收后继续关闭流程
    void pollPseudoConsoleClose();          ///< 轮询伪控制台关闭与 reader 退出
    void deliverClosingOutput();            ///< 关闭期间尽力投递剩余输出
    void finalizeClose();                   ///< 关闭流程收尾：join 线程并释放句柄
    void transition(State state);           ///< 状态迁移并发信号
    void finish(quint32 exitCode, TransportExitReason reason); ///< 发 exited + closed 信号
    void failStart(const QString& error);   ///< 启动失败收尾
    void closeNativeHandle(WinHandle& handle, const QString& operation); ///< 关闭并释放原生句柄
    bool terminateStartupProcess(StartupResources& resources, QString& error); ///< 启动失败回滚

    LocalShellConfig _config;
    State _state{State::Idle};
    int _requestedColumns{80};
    int _requestedRows{24};

    WinHandle _inputWrite;
    WinHandle _outputRead;
    WinHandle _process;
    WinHandle _primaryThread;
    WinHandle _job;
    PseudoConsoleHandle _pseudoConsole;

    std::thread _readerThread;
    std::thread _writerThread;
    std::thread _processWaitThread;
    std::thread _pseudoConsoleCloser;
    std::shared_ptr<AsyncPseudoConsoleClose> _pseudoCloseState;
    int _pseudoClosePolls{0};
    int _pseudoCloserRetryPolls{0};
    int _pseudoCloserRetryIntervalPolls{10};
    int _writerClosePolls{0};
    int _readerClosePolls{0};
    bool _pseudoCloseTimeoutReported{false};
    bool _pseudoCloserStartFailureReported{false};
    bool _writerCloseTimeoutReported{false};
    bool _writerWaitFailureReported{false};
    bool _discardClosingOutput{false};
    bool _pipeTerminationPending{false};
    int _deliveryInFlightPolls{0};

    std::mutex _inputMutex;
    std::condition_variable _inputChanged;
    std::deque<QByteArray> _inputQueue;
    std::size_t _inputBytes{0};
    bool _acceptingInput{false};
    bool _writerStopping{false};

    std::mutex _outputMutex;
    std::condition_variable _outputChanged;
    std::deque<QByteArray> _outputQueue;
    std::size_t _outputBytes{0};
    std::atomic<bool> _readPaused{false};
    bool _outputDrainScheduled{false};
    bool _deliveryInFlight{false};

    std::atomic<bool> _closing{false};
    std::atomic<bool> _readerStopping{false};
    std::atomic<bool> _readerFinished{true};
    std::atomic<bool> _userCloseRequested{false};
    std::atomic<bool> _processWaitStopping{false};
    std::atomic<bool> _ioFailureReported{false};
    bool _exitEmitted{false};
    quint32 _observedExitCode{0};
    TransportExitReason _observedExitReason{TransportExitReason::NormalExit};
};

/**
 * @brief 按微软规则转义单个命令行参数。
 * @param argument 原始参数。
 * @return 带引号与反斜杠转义的参数。
 */
[[nodiscard]] QString quoteWindowsArgument(const QString& argument);

/**
 * @brief 拼接可执行路径与参数为完整命令行。
 * @param executable 可执行路径。
 * @param arguments  参数列表。
 * @return 空格连接的完整命令行（各参数已转义）。
 */
[[nodiscard]] QString buildWindowsCommandLine(const QString& executable,
                                              const QStringList& arguments);

/**
 * @brief 将环境变量转为 Windows 进程环境块（测试用）。
 * @param environment 环境变量。
 * @return 以双 NUL 结尾的环境块（字节视图）。
 */
[[nodiscard]] QByteArray environmentBlockForTest(const QProcessEnvironment& environment);

#ifdef NOVATERM_CONPTY_TESTING
/**
 * @brief ConPTY 启动失败注入阶段（仅测试构建）。
 */
enum class ConPtyFailureStage
{
    None,
    InputPipe,
    OutputPipe,
    PseudoConsole,
    AttributeProbe,
    AttributeInitialize,
    AttributeUpdate,
    ProcessCreate,
    JobCreate,
    JobConfigure,
    JobAssign,
    ResumeThread,
    WorkerThreads,
    ReaderThread,
    WriterThread,
    ProcessWaitThread,
};
void setConPtyFailureStageForTest(ConPtyFailureStage stage);
#endif

} // namespace NovaTerm::Windows
