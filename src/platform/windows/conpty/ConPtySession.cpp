/**
 * @file   ConPtySession.cpp
 * @brief  ConPTY 会话实现：启动、I/O 线程与有序关闭。
 *
 * 启动阶段建立管道、伪控制台与进程（挂起→Job→恢复）。三个工作线程
 * 分别负责读、写、进程等待。关闭流程顺序回收：writer→输入管道→进程→
 * 伪控制台（专用 closer 线程）→reader，避免队列满与 ConPTY 管道满死锁。
 */
#include "ConPtySession.h"

#include "core/ThreadNaming.h"

#include <QMetaObject>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

namespace NovaTerm::Windows {
namespace {

constexpr quint32 UnknownExitCode = (std::numeric_limits<quint32>::max)();

// 关闭链的轮询周期固定为 2 ms（pollWriterClose / pollPseudoConsoleClose），
// 下面的预算都按它折算，并与该文件既有的 5 s 等待（2500 次，对应 writer
// 与 ClosePseudoConsole）同一量级。
//   · ReaderStopGracePolls  ≈  2 s —— 置停止标志后的宽限，让 reader 自行收尾。
//   · ReaderAbortPolls      ≈ 10 s —— 硬失败门槛：终止进程树并打断在途
//                                      ReadFile 之后仍未退出，就不再轮询。
constexpr int ReaderStopGracePolls = 1000;
constexpr int ReaderAbortPolls = 5000;

#ifdef NOVATERM_CONPTY_TESTING
std::atomic<ConPtyFailureStage> failureStage{ConPtyFailureStage::None};

bool injectFailure(ConPtyFailureStage stage, QString& error)
{
    if (failureStage.load(std::memory_order_acquire) != stage)
        return false;
    error = QStringLiteral("Injected ConPTY startup failure at stage %1")
                .arg(static_cast<int>(stage));
    return true;
}
#endif

QString resolveExecutable(const QString& executable, QString& error)
{
    const std::wstring source = executable.toStdWString();
    const DWORD required = SearchPathW(nullptr, source.c_str(), L".exe", 0,
                                      nullptr, nullptr);
    if (required == 0) {
        error = QStringLiteral("Cannot resolve executable '%1': %2")
                    .arg(executable, windowsErrorMessage(GetLastError()));
        return {};
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
    const DWORD length = SearchPathW(nullptr, source.c_str(), L".exe",
                                    static_cast<DWORD>(buffer.size()),
                                    buffer.data(), nullptr);
    if (length == 0 || length >= buffer.size()) {
        error = QStringLiteral("Cannot resolve executable '%1': %2")
                    .arg(executable, windowsErrorMessage(GetLastError()));
        return {};
    }
    return QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length));
}

std::vector<wchar_t> buildEnvironmentBlock(const QProcessEnvironment& environment)
{
    QStringList entries;
    entries.reserve(environment.keys().size());
    for (const QString& key : environment.keys())
        entries.append(key + QLatin1Char('=') + environment.value(key));
    std::sort(entries.begin(), entries.end(), [](const QString& left, const QString& right) {
        return QString::compare(left, right, Qt::CaseInsensitive) < 0;
    });

    std::vector<wchar_t> block;
    for (const QString& entry : std::as_const(entries)) {
        const std::wstring value = entry.toStdWString();
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.isEmpty())
        block.push_back(L'\0');
    return block;
}

class AttributeList final
{
public:
    explicit AttributeList(SIZE_T bytes) : _storage(bytes) {}
    ~AttributeList()
    {
        if (_initialized)
            DeleteProcThreadAttributeList(get());
    }

    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    LPPROC_THREAD_ATTRIBUTE_LIST get()
    {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_storage.data());
    }
    void setInitialized() { _initialized = true; }

private:
    std::vector<std::byte> _storage;
    bool _initialized{false};
};

} // namespace

struct ConPtySession::StartupResources
{
    WinHandle inputRead;
    WinHandle inputWrite;
    WinHandle outputRead;
    WinHandle outputWrite;
    WinHandle process;
    WinHandle primaryThread;
    WinHandle job;
    PseudoConsoleHandle pseudoConsole;
};

struct AsyncPseudoConsoleClose
{
    std::mutex mutex;
    bool closed{false};
};

QString quoteWindowsArgument(const QString& argument)
{
    bool requiresQuotes = argument.isEmpty() || argument.contains(QLatin1Char('"'));
    for (const QChar character : argument)
        requiresQuotes = requiresQuotes || character.isSpace();
    if (!requiresQuotes)
        return argument;

    QString result(QLatin1Char('"'));
    qsizetype backslashes = 0;
    for (const QChar character : argument) {
        if (character == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (character == QLatin1Char('"')) {
            result.append(QString(backslashes * 2 + 1, QLatin1Char('\\')));
            result.append(character);
            backslashes = 0;
            continue;
        }
        result.append(QString(backslashes, QLatin1Char('\\')));
        backslashes = 0;
        result.append(character);
    }
    result.append(QString(backslashes * 2, QLatin1Char('\\')));
    result.append(QLatin1Char('"'));
    return result;
}

QString buildWindowsCommandLine(const QString& executable,
                                const QStringList& arguments)
{
    QStringList quoted;
    quoted.reserve(arguments.size() + 1);
    quoted.append(quoteWindowsArgument(executable));
    for (const QString& argument : arguments)
        quoted.append(quoteWindowsArgument(argument));
    return quoted.join(QLatin1Char(' '));
}

QByteArray environmentBlockForTest(const QProcessEnvironment& environment)
{
    const auto block = buildEnvironmentBlock(environment);
    return QByteArray(reinterpret_cast<const char*>(block.data()),
                      static_cast<qsizetype>(block.size() * sizeof(wchar_t)));
}

#ifdef NOVATERM_CONPTY_TESTING
void setConPtyFailureStageForTest(ConPtyFailureStage stage)
{
    failureStage.store(stage, std::memory_order_release);
}
#endif

ConPtySession::ConPtySession(LocalShellConfig config, int columns, int rows,
                             QObject* parent)
    : QObject(parent)
    , _config(std::move(config))
    , _requestedColumns(columns)
    , _requestedRows(rows)
{
}

ConPtySession::~ConPtySession()
{
    // 外部契约：所有者在 closed() 之后才析构本对象。若契约被打破
    // （生命周期线程事件循环停摆、轮询链中断），release 构建里
    // joinable 的 std::thread 析构会直接 std::terminate。这里防御性
    // 收尾：置停止标志、终止进程树打断阻塞的 ReadFile 与
    // ClosePseudoConsole，再 join 仍存活的线程。
    if (_writerThread.joinable() || _readerThread.joinable()
        || _processWaitThread.joinable() || _pseudoConsoleCloser.joinable()) {
        {
            std::lock_guard<std::mutex> lock(_inputMutex);
            _writerStopping = true;
        }
        _inputChanged.notify_all();
        _readerStopping.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(_outputMutex);
            _outputChanged.notify_all();
        }
        // 终止进程树使 ConPTY 管道中断：阻塞在 ReadFile 的 reader 和
        // 等待输出消费的 closer 随之被释放（与 Abort 关闭语义一致）。
        if (_job)
            TerminateJobObject(_job.get(), 1);
        else if (_process)
            TerminateProcess(_process.get(), 1);
        _processWaitStopping.store(true, std::memory_order_release);
        if (_writerThread.joinable())
            _writerThread.join();
        if (_readerThread.joinable())
            _readerThread.join();
        if (_processWaitThread.joinable())
            _processWaitThread.join();
        if (_pseudoConsoleCloser.joinable())
            _pseudoConsoleCloser.join();
    }
}

void ConPtySession::transition(State state)
{
    if (_state == state)
        return;
    _state = state;
    emit stateChanged(state);
}

void ConPtySession::start()
{
    // 本方法约定在生命周期线程上执行（LocalShellTransport 用 QueuedConnection
    // 调进来），因此在这里把 OS 级线程名定下来：Qt 只在 Linux/Unix 上把
    // QThread::objectName 同步到内核线程名，Windows release 构建下不可见。
    setCurrentThreadName("nvterm-conpty");
    if (_state != State::Idle)
        return;
    transition(State::Starting);

    QString error;
    if (!_config.isValid()) {
        failStart(QStringLiteral("Invalid local shell profile"));
        return;
    }
    if (!ConPtyApi::resolve(&error)) {
        failStart(error);
        return;
    }

    StartupResources resources;
    if (!createStartupResources(resources, error)
        || !launchProcess(resources, error)) {
        failStart(error);
        return;
    }

    _inputWrite = std::move(resources.inputWrite);
    _outputRead = std::move(resources.outputRead);
    _process = std::move(resources.process);
    _primaryThread = std::move(resources.primaryThread);
    _job = std::move(resources.job);
    _pseudoConsole = std::move(resources.pseudoConsole);
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        _acceptingInput = true;
        _writerStopping = false;
    }

    try {
#ifdef NOVATERM_CONPTY_TESTING
        if (injectFailure(ConPtyFailureStage::WorkerThreads, error))
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        _readerFinished.store(false, std::memory_order_release);
        if (injectFailure(ConPtyFailureStage::ReaderThread, error))
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#else
        _readerFinished.store(false, std::memory_order_release);
#endif
        _readerThread = std::thread([this] { readerMain(); });
#ifdef NOVATERM_CONPTY_TESTING
        if (injectFailure(ConPtyFailureStage::WriterThread, error))
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#endif
        _writerThread = std::thread([this] { writerMain(); });
#ifdef NOVATERM_CONPTY_TESTING
        if (injectFailure(ConPtyFailureStage::ProcessWaitThread, error))
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#endif
        _processWaitThread = std::thread([this] { processWaitMain(); });
    } catch (const std::system_error& exception) {
        if (!_readerThread.joinable())
            _readerFinished.store(true, std::memory_order_release);
        _observedExitCode = UnknownExitCode;
        _observedExitReason = TransportExitReason::StartFailed;
        emit errorOccurred(QStringLiteral("ConPTY worker creation failed: %1")
                               .arg(QString::fromLocal8Bit(exception.what())));
        close();
        return;
    }

    transition(State::Running);
    emit started();
}

bool ConPtySession::createStartupResources(StartupResources& resources,
                                           QString& error)
{
    if (_requestedColumns <= 0 || _requestedRows <= 0
        || _requestedColumns > (std::numeric_limits<SHORT>::max)()
        || _requestedRows > (std::numeric_limits<SHORT>::max)()) {
        error = QStringLiteral("Invalid ConPTY size %1x%2")
                    .arg(_requestedColumns).arg(_requestedRows);
        return false;
    }

    HANDLE inputRead = nullptr;
    HANDLE inputWrite = nullptr;
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::InputPipe, error))
        return false;
#endif
    if (!CreatePipe(&inputRead, &inputWrite, nullptr, 0)) {
        error = QStringLiteral("CreatePipe(input) failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }
    resources.inputRead.reset(inputRead);
    resources.inputWrite.reset(inputWrite);
    DWORD pipeMode = PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(resources.inputWrite.get(), &pipeMode,
                                 nullptr, nullptr)) {
        error = QStringLiteral("SetNamedPipeHandleState(input) failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }

    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::OutputPipe, error))
        return false;
#endif
    if (!CreatePipe(&outputRead, &outputWrite, nullptr, 0)) {
        error = QStringLiteral("CreatePipe(output) failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }
    resources.outputRead.reset(outputRead);
    resources.outputWrite.reset(outputWrite);
    pipeMode = PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(resources.outputRead.get(), &pipeMode,
                                 nullptr, nullptr)) {
        error = QStringLiteral("SetNamedPipeHandleState(output) failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }

    HPCON pseudoConsole = nullptr;
    const COORD size{static_cast<SHORT>(_requestedColumns),
                     static_cast<SHORT>(_requestedRows)};
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::PseudoConsole, error))
        return false;
#endif
    const HRESULT result = ConPtyApi::create()(
        size, resources.inputRead.get(), resources.outputWrite.get(), 0,
        &pseudoConsole);
    if (FAILED(result)) {
        error = QStringLiteral("CreatePseudoConsole failed: %1")
                    .arg(hresultMessage(result));
        return false;
    }
    resources.pseudoConsole.reset(pseudoConsole);

    // ConPTY 已复制其所需的管道端，宿主必须释放原始端，否则关闭时
    // EOF 无法正确传播。
    resources.inputRead.reset();
    resources.outputWrite.reset();
    return true;
}

bool ConPtySession::launchProcess(StartupResources& resources, QString& error)
{
    const QString executable = resolveExecutable(_config.profile.executable, error);
    if (executable.isEmpty())
        return false;

    SIZE_T attributeBytes = 0;
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::AttributeProbe, error))
        return false;
#endif
    SetLastError(ERROR_SUCCESS);
    const BOOL probeResult = InitializeProcThreadAttributeList(
        nullptr, 1, 0, &attributeBytes);
    const DWORD probeError = GetLastError();
    if (probeResult || probeError != ERROR_INSUFFICIENT_BUFFER
        || attributeBytes == 0) {
        error = QStringLiteral("InitializeProcThreadAttributeList(size) failed: %1")
                    .arg(windowsErrorMessage(probeError));
        return false;
    }

    AttributeList attributes(attributeBytes);
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::AttributeInitialize, error))
        return false;
#endif
    if (!InitializeProcThreadAttributeList(attributes.get(), 1, 0,
                                           &attributeBytes)) {
        error = QStringLiteral("InitializeProcThreadAttributeList failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }
    attributes.setInitialized();
    HPCON pseudoConsole = resources.pseudoConsole.get();
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::AttributeUpdate, error))
        return false;
#endif
    if (!UpdateProcThreadAttribute(attributes.get(), 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudoConsole, sizeof(pseudoConsole),
                                   nullptr, nullptr)) {
        error = QStringLiteral("UpdateProcThreadAttribute failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.get();

    const QString commandLine = buildWindowsCommandLine(
        executable, _config.profile.arguments);
    std::vector<wchar_t> command(commandLine.size() + 1U);
    commandLine.toWCharArray(command.data());
    command.back() = L'\0';

    const std::vector<wchar_t> environmentBlock = buildEnvironmentBlock(
        _config.mergedEnvironment());
    const QString workingDirectory = _config.effectiveWorkingDirectory();
    const std::wstring application = executable.toStdWString();
    const std::wstring currentDirectory = workingDirectory.toStdWString();
    PROCESS_INFORMATION processInfo{};
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::ProcessCreate, error))
        return false;
#endif
    const BOOL created = CreateProcessW(
        application.c_str(), command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT
            | CREATE_SUSPENDED,
        const_cast<wchar_t*>(environmentBlock.data()),
        currentDirectory.empty() ? nullptr : currentDirectory.c_str(),
        &startup.StartupInfo, &processInfo);
    if (!created) {
        error = QStringLiteral("CreateProcessW failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        return false;
    }
    resources.process.reset(processInfo.hProcess);
    resources.primaryThread.reset(processInfo.hThread);

#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::JobCreate, error)) {
        terminateStartupProcess(resources, error);
        return false;
    }
#endif
    resources.job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!resources.job) {
        error = QStringLiteral("CreateJobObjectW failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        terminateStartupProcess(resources, error);
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::JobConfigure, error)) {
        terminateStartupProcess(resources, error);
        return false;
    }
#endif
    if (!SetInformationJobObject(resources.job.get(),
                                 JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        error = QStringLiteral("SetInformationJobObject failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        terminateStartupProcess(resources, error);
        return false;
    }
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::JobAssign, error)) {
        terminateStartupProcess(resources, error);
        return false;
    }
#endif
    if (!AssignProcessToJobObject(resources.job.get(), resources.process.get())) {
        error = QStringLiteral("AssignProcessToJobObject failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        terminateStartupProcess(resources, error);
        return false;
    }
#ifdef NOVATERM_CONPTY_TESTING
    if (injectFailure(ConPtyFailureStage::ResumeThread, error)) {
        terminateStartupProcess(resources, error);
        return false;
    }
#endif
    if (ResumeThread(resources.primaryThread.get()) == static_cast<DWORD>(-1)) {
        error = QStringLiteral("ResumeThread failed: %1")
                    .arg(windowsErrorMessage(GetLastError()));
        terminateStartupProcess(resources, error);
        return false;
    }
    return true;
}

bool ConPtySession::terminateStartupProcess(StartupResources& resources,
                                            QString& error)
{
    bool cleaned = true;
    if (resources.job) {
        const HANDLE job = resources.job.get();
        if (!CloseHandle(job)) {
            error += QStringLiteral("; rollback CloseHandle(job) failed: %1")
                         .arg(windowsErrorMessage(GetLastError()));
            cleaned = false;
        } else {
            (void)resources.job.release();
        }
    }
    if (resources.process) {
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(resources.process.get(), &exitCode)) {
            error += QStringLiteral("; rollback GetExitCodeProcess failed: %1")
                         .arg(windowsErrorMessage(GetLastError()));
            cleaned = false;
        }
        if (exitCode == STILL_ACTIVE
            && !TerminateProcess(resources.process.get(), 1)) {
            const DWORD terminateError = GetLastError();
            if (terminateError != ERROR_ACCESS_DENIED) {
                error += QStringLiteral("; rollback TerminateProcess failed: %1")
                             .arg(windowsErrorMessage(terminateError));
                cleaned = false;
            }
        }
        const DWORD waitResult = WaitForSingleObject(resources.process.get(), 5000);
        if (waitResult != WAIT_OBJECT_0) {
            const DWORD waitError = waitResult == WAIT_FAILED
                ? GetLastError() : WAIT_TIMEOUT;
            error += QStringLiteral("; rollback process wait failed: %1")
                         .arg(windowsErrorMessage(waitError));
            cleaned = false;
        }
    }
    return cleaned;
}

void ConPtySession::closeNativeHandle(WinHandle& handle,
                                      const QString& operation)
{
    const HANDLE nativeHandle = handle.get();
    if (!nativeHandle)
        return;
    if (!CloseHandle(nativeHandle)) {
        emit errorOccurred(QStringLiteral("%1 failed: %2")
                               .arg(operation,
                                    windowsErrorMessage(GetLastError())));
        return;
    }
    (void)handle.release();
}

bool ConPtySession::tryEnqueueInput(const QByteArray& data)
{
    if (data.isEmpty())
        return true;
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        if (!_acceptingInput)
            return false;
        const std::size_t size = static_cast<std::size_t>(data.size());
        if (size > InputCapacity || _inputBytes > InputCapacity - size)
            return false;
        _inputQueue.push_back(data);
        _inputBytes += size;
    }
    _inputChanged.notify_one();
    return true;
}

void ConPtySession::writerMain()
{
    setCurrentThreadName("conpty-writer");
    auto retryDelay = std::chrono::milliseconds(1);
    for (;;) {
        QByteArray data;
        {
            std::unique_lock<std::mutex> lock(_inputMutex);
            _inputChanged.wait(lock, [this] {
                return _writerStopping || !_inputQueue.empty();
            });
            if (_inputQueue.empty()) {
                if (_writerStopping)
                    return;
                continue;
            }
            if (_writerStopping)
                return;
            data = std::move(_inputQueue.front());
            _inputQueue.pop_front();
            _inputBytes -= static_cast<std::size_t>(data.size());
        }

        qsizetype offset = 0;
        while (offset < data.size()) {
            {
                std::lock_guard<std::mutex> lock(_inputMutex);
                if (_writerStopping)
                    return;
            }
            const DWORD requested = static_cast<DWORD>((std::min)(
                qsizetype((std::numeric_limits<DWORD>::max)()),
                data.size() - offset));
            DWORD written = 0;
            const BOOL ok = WriteFile(_inputWrite.get(), data.constData() + offset,
                                      requested, &written, nullptr);
            if (!ok || written == 0) {
                const DWORD error = ok ? ERROR_NO_DATA : GetLastError();
                if (error == ERROR_NO_DATA) {
                    const DWORD processState = WaitForSingleObject(_process.get(), 0);
                    if (processState == WAIT_TIMEOUT) {
                        std::this_thread::sleep_for(retryDelay);
                        retryDelay = (std::min)(retryDelay * 2,
                                                std::chrono::milliseconds(20));
                        continue;
                    }
                    if (processState == WAIT_FAILED) {
                        const DWORD waitError = GetLastError();
                        QMetaObject::invokeMethod(this, [this, waitError] {
                            handleIoFailure(QStringLiteral("WaitForSingleObject(writer)"),
                                            waitError);
                        }, Qt::QueuedConnection);
                    }
                    return;
                }
                const bool peerClosed = error == ERROR_BROKEN_PIPE
                    || error == ERROR_PIPE_NOT_CONNECTED;
                if (peerClosed || error == ERROR_OPERATION_ABORTED) {
                    QMetaObject::invokeMethod(this, [this, error] {
                        handlePipeTermination(QStringLiteral("WriteFile"), error);
                    }, Qt::QueuedConnection);
                } else {
                    QMetaObject::invokeMethod(this, [this, error] {
                        handleIoFailure(QStringLiteral("WriteFile"), error);
                    }, Qt::QueuedConnection);
                }
                return;
            }
            offset += static_cast<qsizetype>(written);
            retryDelay = std::chrono::milliseconds(1);
        }
    }
}

void ConPtySession::readerMain()
{
    setCurrentThreadName("conpty-reader");
    const auto finishedGuard = qScopeGuard([this] {
        _readerFinished.store(true, std::memory_order_release);
        _outputChanged.notify_all();
    });
    std::array<char, ReadBufferSize> buffer{};
    auto retryDelay = std::chrono::milliseconds(1);
    for (;;) {
        if (_readerStopping.load(std::memory_order_acquire))
            return;
        DWORD bytesRead = 0;
        const BOOL ok = ReadFile(_outputRead.get(), buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            const DWORD error = ok ? ERROR_NO_DATA : GetLastError();
            if (error == ERROR_NO_DATA) {
                std::this_thread::sleep_for(retryDelay);
                retryDelay = (std::min)(retryDelay * 2,
                                        std::chrono::milliseconds(20));
                continue;
            }
            const bool pipeTermination = error == ERROR_BROKEN_PIPE
                || error == ERROR_PIPE_NOT_CONNECTED
                || error == ERROR_OPERATION_ABORTED;
            if (!ok && pipeTermination) {
                QMetaObject::invokeMethod(this, [this, error] {
                    handlePipeTermination(QStringLiteral("ReadFile"), error);
                }, Qt::QueuedConnection);
            } else if (!ok) {
                QMetaObject::invokeMethod(this, [this, error] {
                    handleIoFailure(QStringLiteral("ReadFile"), error);
                }, Qt::QueuedConnection);
            }
            return;
        }
        retryDelay = std::chrono::milliseconds(1);

        QByteArray data(buffer.data(), static_cast<qsizetype>(bytesRead));
        {
            std::unique_lock<std::mutex> lock(_outputMutex);
            // 谓词必须包含停止标志：正常关闭靠 deliverClosingOutput 清空
            // 队列唤醒 reader，但那条 rescue 链一旦中断（生命周期线程
            // 事件循环停摆），队列满时 reader 会永久阻塞在这里，进而
            // 让 joinable 的线程活到析构。
            _outputChanged.wait(lock, [this, bytesRead] {
                return _readerStopping.load(std::memory_order_acquire)
                    || _outputBytes <= OutputCapacity - bytesRead;
            });
            if (_readerStopping.load(std::memory_order_acquire)) {
                // 关闭中断路径：丢弃本批数据让 reader 尽快退出；
                // 退出时 finishedGuard 会置 _readerFinished 并唤醒等待方。
                return;
            }
            _outputBytes += bytesRead;
            _outputQueue.push_back(std::move(data));
        }
        scheduleOutputDrain();
    }
}

void ConPtySession::scheduleOutputDrain()
{
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        if (!_outputDrainScheduled) {
            _outputDrainScheduled = true;
            schedule = true;
        }
    }
    if (schedule) {
        QMetaObject::invokeMethod(this, [this] { drainOutput(); },
                                  Qt::QueuedConnection);
    }
}

void ConPtySession::drainOutput()
{
    if (_readPaused.load(std::memory_order_acquire))
        return;

    QByteArray batch;
    bool more = false;
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        if (_deliveryInFlight)
            return;
        while (!_outputQueue.empty()
               && static_cast<std::size_t>(batch.size()) < OutputDeliveryBatch) {
            QByteArray& front = _outputQueue.front();
            _outputBytes -= static_cast<std::size_t>(front.size());
            batch.append(front);
            _outputQueue.pop_front();
        }
        more = !_outputQueue.empty();
        _deliveryInFlight = !batch.isEmpty();
        if (batch.isEmpty() && !more)
            _outputDrainScheduled = false;
    }
    _outputChanged.notify_all();
    if (!batch.isEmpty())
        emit dataReady(batch);
}

void ConPtySession::acknowledgeOutput()
{
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        _deliveryInFlight = false;
        if (_outputQueue.empty())
            _outputDrainScheduled = false;
    }
    if (!_closing.load(std::memory_order_acquire)) {
        QMetaObject::invokeMethod(this, [this] { drainOutput(); },
                                  Qt::QueuedConnection);
    }
}

void ConPtySession::setReadPaused(bool paused)
{
    _readPaused.store(paused, std::memory_order_release);
    if (!paused)
        drainOutput();
}

void ConPtySession::requestClose()
{
    _userCloseRequested.store(true, std::memory_order_release);
    _observedExitCode = UnknownExitCode;
    _observedExitReason = TransportExitReason::UserClosed;
    close();
}

void ConPtySession::resize(int columns, int rows)
{
    _requestedColumns = columns;
    _requestedRows = rows;
    if (_state != State::Running)
        return;
    if (columns <= 0 || rows <= 0
        || columns > (std::numeric_limits<SHORT>::max)()
        || rows > (std::numeric_limits<SHORT>::max)()) {
        emit errorOccurred(QStringLiteral("Invalid ConPTY resize %1x%2")
                               .arg(columns).arg(rows));
        return;
    }
    const COORD size{static_cast<SHORT>(columns), static_cast<SHORT>(rows)};
    const HRESULT result = ConPtyApi::resize()(_pseudoConsole.get(), size);
    if (FAILED(result)) {
        emit errorOccurred(QStringLiteral("ResizePseudoConsole failed: %1")
                               .arg(hresultMessage(result)));
        return;
    }
}

void ConPtySession::processWaitMain()
{
    setCurrentThreadName("conpty-wait");
    DWORD result = WAIT_TIMEOUT;
    while (!_processWaitStopping.load(std::memory_order_acquire)) {
        result = WaitForSingleObject(_process.get(), 100);
        if (result != WAIT_TIMEOUT)
            break;
    }
    if (_processWaitStopping.load(std::memory_order_acquire)
        && result == WAIT_TIMEOUT) {
        return;
    }
    if (result != WAIT_OBJECT_0) {
        const DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        QMetaObject::invokeMethod(this, [this, error] {
            handleIoFailure(QStringLiteral("WaitForSingleObject"), error);
        }, Qt::QueuedConnection);
        return;
    }
    DWORD exitCode = UnknownExitCode;
    if (!GetExitCodeProcess(_process.get(), &exitCode)) {
        const DWORD error = GetLastError();
        QMetaObject::invokeMethod(this, [this, error] {
            handleIoFailure(QStringLiteral("GetExitCodeProcess"), error);
        }, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(this, [this, exitCode] {
        handleProcessExit(exitCode);
    }, Qt::QueuedConnection);
}

void ConPtySession::handleProcessExit(quint32 exitCode)
{
    if (_state == State::Closed)
        return;
    _observedExitCode = exitCode;
    _observedExitReason = _userCloseRequested.load(std::memory_order_acquire)
        ? TransportExitReason::UserClosed
        : exitCode == 0 ? TransportExitReason::NormalExit
        : exitCode >= 0xC0000000U ? TransportExitReason::Crash
                                  : TransportExitReason::FailedExit;
    close();
}

void ConPtySession::handleIoFailure(const QString& operation, unsigned long error)
{
    if (_state == State::Closing || _state == State::Closed)
        return;
    if (!_ioFailureReported.exchange(true, std::memory_order_acq_rel))
        emit errorOccurred(QStringLiteral("%1 failed: %2")
                               .arg(operation, windowsErrorMessage(error)));
    _observedExitCode = UnknownExitCode;
    _observedExitReason = TransportExitReason::IoError;
    close();
}

void ConPtySession::handlePipeTermination(const QString& operation,
                                          unsigned long error)
{
    if (_state == State::Closing || _state == State::Closed)
        return;
    if (_pipeTerminationPending)
        return;
    _pipeTerminationPending = true;
    QTimer::singleShot(std::chrono::milliseconds(50), this,
                       [this, operation, error] {
        confirmPipeTermination(operation, error);
    });
}

void ConPtySession::confirmPipeTermination(const QString& operation,
                                           unsigned long error)
{
    _pipeTerminationPending = false;
    if (_state == State::Closing || _state == State::Closed)
        return;
    DWORD exitCode = STILL_ACTIVE;
    if (!_process) {
        handleIoFailure(QStringLiteral("GetExitCodeProcess(%1)").arg(operation),
                        ERROR_INVALID_HANDLE);
    } else if (!GetExitCodeProcess(_process.get(), &exitCode)) {
        handleIoFailure(QStringLiteral("GetExitCodeProcess(%1)").arg(operation),
                        GetLastError());
    } else if (exitCode == STILL_ACTIVE) {
        handleIoFailure(operation, error);
    } else {
        handleProcessExit(exitCode);
    }
}

void ConPtySession::close()
{
    if (_state == State::Closed || _state == State::Closing)
        return;
    if (_state == State::Idle) {
        transition(State::Closed);
        finish(UnknownExitCode, TransportExitReason::UserClosed);
        return;
    }
    transition(State::Closing);
    _closing.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        _acceptingInput = false;
        _writerStopping = true;
        _inputQueue.clear();
        _inputBytes = 0;
    }
    _inputChanged.notify_all();
    pollWriterClose();
}

void ConPtySession::pollWriterClose()
{
    if (_writerThread.joinable()) {
        const DWORD threadWait = WaitForSingleObject(_writerThread.native_handle(), 0);
        if (threadWait == WAIT_TIMEOUT) {
            if (++_writerClosePolls >= 2500 && !_writerCloseTimeoutReported) {
                _writerCloseTimeoutReported = true;
                emit errorOccurred(QStringLiteral("Writer thread shutdown is still pending"));
            }
            QTimer::singleShot(std::chrono::milliseconds(2), this,
                               &ConPtySession::pollWriterClose);
            return;
        }
        if (threadWait == WAIT_FAILED) {
            const DWORD waitError = GetLastError();
            DWORD threadExitCode = STILL_ACTIVE;
            if (GetExitCodeThread(_writerThread.native_handle(), &threadExitCode)
                && threadExitCode != STILL_ACTIVE) {
                _writerThread.join();
                continueCloseAfterWriter();
                return;
            }
            if (!_writerWaitFailureReported) {
                _writerWaitFailureReported = true;
                emit errorOccurred(QStringLiteral("Writer thread wait failed: %1")
                                       .arg(windowsErrorMessage(waitError)));
            }
            QTimer::singleShot(std::chrono::milliseconds(20), this,
                               &ConPtySession::pollWriterClose);
            return;
        }
        _writerThread.join();
    }
    continueCloseAfterWriter();
}

void ConPtySession::continueCloseAfterWriter()
{
    closeNativeHandle(_inputWrite, QStringLiteral("CloseHandle(input pipe)"));

    DWORD processExitCode = STILL_ACTIVE;
    if (_process && !GetExitCodeProcess(_process.get(), &processExitCode)) {
        emit errorOccurred(QStringLiteral("GetExitCodeProcess(close) failed: %1")
                               .arg(windowsErrorMessage(GetLastError())));
    }
    if (_process && processExitCode == STILL_ACTIVE) {
        // 关闭 kill-on-close Job 会终止整个 shell 进程树，包括 Clink
        // 辅助进程及继承了 ConPTY 的孙进程。
        closeNativeHandle(_job, QStringLiteral("CloseHandle(job)"));
    } else if (processExitCode != STILL_ACTIVE) {
        closeNativeHandle(_job, QStringLiteral("CloseHandle(job)"));
    }
    if (_process) {
        DWORD waitResult = WaitForSingleObject(_process.get(), 5000);
        if (waitResult != WAIT_OBJECT_0) {
            const DWORD error = waitResult == WAIT_FAILED ? GetLastError() : WAIT_TIMEOUT;
            emit errorOccurred(QStringLiteral("Process shutdown wait failed: %1")
                                   .arg(windowsErrorMessage(error)));
            if (!TerminateProcess(_process.get(), 1)) {
                const DWORD terminateError = GetLastError();
                if (terminateError != ERROR_ACCESS_DENIED) {
                    emit errorOccurred(QStringLiteral("TerminateProcess fallback failed: %1")
                                           .arg(windowsErrorMessage(terminateError)));
                }
            }
            waitResult = WaitForSingleObject(_process.get(), 5000);
            if (waitResult != WAIT_OBJECT_0) {
                const DWORD finalError = waitResult == WAIT_FAILED
                    ? GetLastError() : WAIT_TIMEOUT;
                emit errorOccurred(QStringLiteral("Final process wait failed: %1")
                                       .arg(windowsErrorMessage(finalError)));
            }
        }
        DWORD actualExitCode = UnknownExitCode;
        if (waitResult == WAIT_OBJECT_0) {
            if (GetExitCodeProcess(_process.get(), &actualExitCode)) {
                _observedExitCode = actualExitCode;
            } else {
                emit errorOccurred(QStringLiteral("GetExitCodeProcess(final) failed: %1")
                                       .arg(windowsErrorMessage(GetLastError())));
            }
        }
    }

    _readPaused.store(false, std::memory_order_release);
    _outputChanged.notify_all();

    // ClosePseudoConsole 可能同步等待客户端与输出消费。放在专用 closer 线程
    // 执行，本生命周期线程继续 drain 有界 reader 队列；否则队列满与
    // ConPTY 输出管道满会互相死锁导致关闭卡死。
    _pseudoCloseState = std::make_shared<AsyncPseudoConsoleClose>();
    _pseudoCloseState->closed = !_pseudoConsole;
    tryStartPseudoConsoleCloser();
    pollPseudoConsoleClose();
}

void ConPtySession::tryStartPseudoConsoleCloser()
{
    if (!_pseudoConsole || _pseudoConsoleCloser.joinable())
        return;
    const HPCON pseudoConsole = _pseudoConsole.get();
    try {
        const auto closeState = _pseudoCloseState;
        _pseudoConsoleCloser = std::thread([pseudoConsole, closeState] {
            setCurrentThreadName("conpty-close");
            ConPtyApi::close()(pseudoConsole);
            std::lock_guard<std::mutex> lock(closeState->mutex);
            closeState->closed = true;
        });
        (void)_pseudoConsole.release();
        _pseudoCloserRetryPolls = 0;
        _pseudoCloserRetryIntervalPolls = 10;
        _pseudoClosePolls = 0;
        _pseudoCloseTimeoutReported = false;
    } catch (const std::system_error& exception) {
        _pseudoCloserRetryPolls = _pseudoCloserRetryIntervalPolls;
        _pseudoCloserRetryIntervalPolls = (std::min)(
            _pseudoCloserRetryIntervalPolls * 2, 500);
        if (!_pseudoCloserStartFailureReported) {
            _pseudoCloserStartFailureReported = true;
            emit errorOccurred(QStringLiteral("ConPTY closer creation failed; will retry: %1")
                                   .arg(QString::fromLocal8Bit(exception.what())));
        }
    }
}

void ConPtySession::deliverClosingOutput()
{
    std::unique_lock<std::mutex> lock(_outputMutex);
    if (_discardClosingOutput) {
        _deliveryInFlight = false;
        _outputQueue.clear();
        _outputBytes = 0;
        _outputChanged.notify_all();
        return;
    }
    if (_deliveryInFlight) {
        if (++_deliveryInFlightPolls >= 250) {
            _discardClosingOutput = true;
            _deliveryInFlight = false;
            _outputQueue.clear();
            _outputBytes = 0;
            _outputChanged.notify_all();
        }
        return;
    }
    _deliveryInFlightPolls = 0;
    lock.unlock();
    drainOutput();
}

void ConPtySession::requestReaderAbort()
{
    _readerStopping.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        _outputChanged.notify_all();
    }
    if (_readerAbortRequested)
        return;
    // 一次性升级：置停止标志只能让 reader 从输出队列的等待里脱身，它仍
    // 可能阻塞在 ReadFile 上，因此再从外部打断那个系统调用。
    _readerAbortRequested = true;
    // 终止进程树：ConPTY 与子进程的写端随之关闭，ReadFile 返回 0 或
    // ERROR_BROKEN_PIPE，reader 从循环顶部看到停止标志后退出。
    if (_job)
        TerminateJobObject(_job.get(), 1);
    else if (_process)
        TerminateProcess(_process.get(), 1);
    if (_readerThread.joinable()) {
        // CancelSynchronousIo 只针对该线程发出的同步 I/O，正是这里要打
        // 断的那一次 ReadFile（std::thread 的 native_handle 在 MSVC 上
        // 就是线程 HANDLE，本文件 pollWriterClose 已按同样方式使用）。
        CancelSynchronousIo(_readerThread.native_handle());
    }
    // 兜底：取消已排入内核、在 CancelSynchronousIo 之后才到达的请求。
    if (_outputRead)
        CancelIoEx(_outputRead.get(), nullptr);
}

void ConPtySession::pollPseudoConsoleClose()
{
    deliverClosingOutput();
    bool pseudoClosed = false;
    {
        std::lock_guard<std::mutex> lock(_pseudoCloseState->mutex);
        pseudoClosed = _pseudoCloseState->closed;
    }
    if (!pseudoClosed) {
        if (_pseudoCloserRetryPolls > 0)
            --_pseudoCloserRetryPolls;
        else
            tryStartPseudoConsoleCloser();
        if (_pseudoConsoleCloser.joinable()
            && ++_pseudoClosePolls >= 2500 && !_pseudoCloseTimeoutReported) {
            _pseudoCloseTimeoutReported = true;
            emit errorOccurred(QStringLiteral("ClosePseudoConsole is still pending"));
        }
        QTimer::singleShot(std::chrono::milliseconds(2), this,
                           &ConPtySession::pollPseudoConsoleClose);
        return;
    }
    if (_pseudoConsoleCloser.joinable())
        _pseudoConsoleCloser.join();

    if (!_readerFinished.load(std::memory_order_acquire)) {
        if (++_readerClosePolls >= ReaderStopGracePolls)
            requestReaderAbort();
        if (_readerClosePolls < ReaderAbortPolls) {
            QTimer::singleShot(std::chrono::milliseconds(2), this,
                               &ConPtySession::pollPseudoConsoleClose);
            return;
        }
        // 硬失败：reader 在宽限期与终止/CancelSynchronousIo 之后仍未退出
        // （rescue 链被外部打断时的防御分支）。继续 2 ms 轮询只会让生命
        // 周期线程永久空转且永远到不了 finalizeClose()，因此就此转入收尾：
        // 跳过 join 把它留给析构里既有的防御性收尾（那里同样先终止进程
        // 树再 join）。绝不 detach —— readerMain 捕获的是裸 this，detach
        // 之后对象被 deleteLater 销毁就是 UAF。
        _readerJoinAbandoned = true;
        emit errorOccurred(QStringLiteral(
            "Reader thread still running; closing without joining it"));
    }
    if (_readerThread.joinable() && !_readerJoinAbandoned)
        _readerThread.join();
    deliverClosingOutput();
    {
        std::lock_guard<std::mutex> lock(_outputMutex);
        if (!_outputQueue.empty() || _deliveryInFlight) {
            if (++_readerClosePolls >= 1250) {
                _discardClosingOutput = true;
                _deliveryInFlight = false;
            }
            QTimer::singleShot(std::chrono::milliseconds(2), this,
                               &ConPtySession::pollPseudoConsoleClose);
            return;
        }
        _outputDrainScheduled = false;
    }
    finalizeClose();
}

void ConPtySession::finalizeClose()
{
    closeNativeHandle(_outputRead, QStringLiteral("CloseHandle(output pipe)"));

    _processWaitStopping.store(true, std::memory_order_release);
    if (_processWaitThread.joinable())
        _processWaitThread.join();
    closeNativeHandle(_primaryThread, QStringLiteral("CloseHandle(primary thread)"));
    closeNativeHandle(_process, QStringLiteral("CloseHandle(process)"));
    closeNativeHandle(_job, QStringLiteral("CloseHandle(job)"));

    transition(State::Closed);
    finish(_observedExitCode, _observedExitReason);
}

void ConPtySession::finish(quint32 exitCode, TransportExitReason reason)
{
    if (!_exitEmitted) {
        _exitEmitted = true;
        emit exited(exitCode, reason);
    }
    emit closed();
}

void ConPtySession::failStart(const QString& error)
{
    transition(State::Closing);
    if (!error.isEmpty())
        emit errorOccurred(error);
    transition(State::Closed);
    finish(UnknownExitCode, TransportExitReason::StartFailed);
}

} // namespace NovaTerm::Windows
