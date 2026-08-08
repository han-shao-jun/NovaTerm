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

class ConPtySession final : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Starting, Running, Closing, Closed };
    Q_ENUM(State)

    explicit ConPtySession(LocalShellConfig config, int columns, int rows,
                           QObject* parent = nullptr);
    ~ConPtySession() override;

    // These two methods are thread-safe GUI-facing handoff operations. They
    // only touch mutex/atomic protected state and never perform native I/O.
    [[nodiscard]] bool tryEnqueueInput(const QByteArray& data);
    void acknowledgeOutput();

public slots:
    void start();
    void resize(int columns, int rows);
    void setReadPaused(bool paused);
    void requestClose();
    void close();

signals:
    void started();
    void dataReady(const QByteArray& data);
    void errorOccurred(const QString& error);
    void exited(quint32 exitCode, TransportExitReason reason);
    void closed();
    void stateChanged(NovaTerm::Windows::ConPtySession::State state);

private:
    static constexpr std::size_t InputCapacity = 4U * 1024U * 1024U;
    static constexpr std::size_t OutputCapacity = 8U * 1024U * 1024U;
    static constexpr std::size_t ReadBufferSize = 64U * 1024U;
    static constexpr std::size_t OutputDeliveryBatch = 256U * 1024U;

    struct StartupResources;

    [[nodiscard]] bool createStartupResources(StartupResources& resources,
                                              QString& error);
    [[nodiscard]] bool launchProcess(StartupResources& resources,
                                     QString& error);
    void readerMain();
    void writerMain();
    void processWaitMain();
    void scheduleOutputDrain();
    void drainOutput();
    void handleProcessExit(quint32 exitCode);
    void handleIoFailure(const QString& operation, unsigned long error);
    void handlePipeTermination(const QString& operation, unsigned long error);
    void confirmPipeTermination(const QString& operation, unsigned long error);
    void tryStartPseudoConsoleCloser();
    void pollWriterClose();
    void continueCloseAfterWriter();
    void pollPseudoConsoleClose();
    void deliverClosingOutput();
    void finalizeClose();
    void transition(State state);
    void finish(quint32 exitCode, TransportExitReason reason);
    void failStart(const QString& error);
    void closeNativeHandle(WinHandle& handle, const QString& operation);
    bool terminateStartupProcess(StartupResources& resources, QString& error);
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

[[nodiscard]] QString quoteWindowsArgument(const QString& argument);
[[nodiscard]] QString buildWindowsCommandLine(const QString& executable,
                                              const QStringList& arguments);
[[nodiscard]] QByteArray environmentBlockForTest(const QProcessEnvironment& environment);

#ifdef NOVATERM_CONPTY_TESTING
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
