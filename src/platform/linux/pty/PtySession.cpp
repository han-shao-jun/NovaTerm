/**
 * @file   PtySession.cpp
 * @brief  Linux PTY 会话实现：fork、I/O 与子进程退出处理。
 *
 * start() 解析可执行路径、组装 argv/environ、openpty + fork + execve，
 * 通过 errorPipe 回传子进程启动失败原因。reader/writer 基于 QSocketNotifier
 * 事件驱动；子进程退出由 25ms QTimer 轮询 waitpid 检测。
 */
#include "PtySession.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QTimer>

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace NovaTerm::Linux {
namespace {

// 无法解析出的退出码哨兵值（取 quint32 最大值）
constexpr quint32 UnknownExitCode = (std::numeric_limits<quint32>::max)();

// 将 errno 转换为本地编码的 QString 错误描述
QString errnoMessage(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}

// 解析可执行文件路径：含 '/' 时按绝对路径校验，否则在 PATH 各目录中搜索
QString resolveExecutable(const QString& program,
                          const QProcessEnvironment& environment)
{
    if (program.contains(QLatin1Char('/'))) {
        const QFileInfo file(program);
        return file.isExecutable() && file.isFile() ? file.absoluteFilePath()
                                                    : QString{};
    }
    const QString path = environment.value(QStringLiteral("PATH"));
    for (const QString& directory : path.split(QLatin1Char(':'), Qt::KeepEmptyParts)) {
        const QFileInfo file(QDir(directory.isEmpty() ? QStringLiteral(".") : directory)
                                 .filePath(program));
        if (file.isFile() && file.isExecutable())
            return file.absoluteFilePath();
    }
    return {};
}

// 为文件描述符追加状态标志（F_GETFL→文件状态 / F_GETFD→描述符标志）
bool setDescriptorFlag(int fd, int command, int flag)
{
    const int current = ::fcntl(fd, command, 0);
    if (current < 0)
        return false;
    const int setCommand = command == F_GETFL ? F_SETFL : F_SETFD;
    return ::fcntl(fd, setCommand, current | flag) == 0;
}

// 子进程专用：将 errno 写入错误管道后立即 _exit(127) 退出
// 使用 127 与 shell 约定一致，表示"找不到可执行文件"
void childFailure(int errorFd, int error)
{
    const char* bytes = reinterpret_cast<const char*>(&error);
    std::size_t offset = 0;
    while (offset < sizeof(error)) {
        const ssize_t written = ::write(errorFd, bytes + offset,
                                        sizeof(error) - offset);
        if (written > 0)
            offset += static_cast<std::size_t>(written);
        else if (written < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    _exit(127);
}

} // namespace

PtySession::PtySession(LocalShellConfig config, int columns, int rows,
                       QObject* parent)
    : QObject(parent)
    , _config(std::move(config))
    , _columns(columns)
    , _rows(rows)
{
}

// 析构时强制清理：关闭描述符，对残留子进程发送 SIGKILL 并回收，避免僵尸进程
PtySession::~PtySession()
{
    closeDescriptors();
    if (_childPid > 0) {
        // 向进程组（负 PID）发信号，连带终止子进程派生的所有子孙进程
        ::kill(-static_cast<pid_t>(_childPid), SIGKILL);
        int status = 0;
        while (::waitpid(static_cast<pid_t>(_childPid), &status, 0) < 0
               && errno == EINTR) {
        }
    }
}

// 状态机切换并广播变更；同状态跳过避免重复信号
void PtySession::transition(State state)
{
    if (_state == state)
        return;
    _state = state;
    emit stateChanged(state);
}

// 启动流程：参数校验 → 解析可执行文件 → 组装 argv/environ → openpty+fork+execve
// 通过 errorPipe 同步获取子进程 execve 失败原因；成功后建立 I/O 通知与退出轮询
void PtySession::start()
{
    if (_state != State::Idle)
        return;
    transition(State::Starting);
    if (!_config.isValid()) {
        failStart(QStringLiteral("Invalid local shell profile"));
        return;
    }
    if (_columns <= 0 || _rows <= 0 || _columns > USHRT_MAX || _rows > USHRT_MAX) {
        failStart(QStringLiteral("Invalid PTY size %1x%2").arg(_columns).arg(_rows));
        return;
    }

    const QProcessEnvironment environment = _config.mergedEnvironment();
    const QString executable = resolveExecutable(_config.profile.executable,
                                                 environment);
    if (executable.isEmpty()) {
        failStart(QStringLiteral("Cannot resolve executable '%1'")
                      .arg(_config.profile.executable));
        return;
    }

    // 组装 argv：[0]=可执行文件路径，后续为参数，末尾 nullptr 收尾
    std::vector<QByteArray> argumentStorage;
    argumentStorage.reserve(static_cast<std::size_t>(_config.profile.arguments.size()) + 1U);
    argumentStorage.push_back(executable.toUtf8());
    for (const QString& argument : _config.profile.arguments)
        argumentStorage.push_back(argument.toUtf8());
    std::vector<char*> arguments;
    arguments.reserve(argumentStorage.size() + 1U);
    for (QByteArray& argument : argumentStorage)
        arguments.push_back(argument.data());
    arguments.push_back(nullptr);

    // 组装 environ：每项形如 KEY=VALUE，末尾 nullptr 收尾
    std::vector<QByteArray> environmentStorage;
    environmentStorage.reserve(static_cast<std::size_t>(environment.keys().size()));
    for (const QString& key : environment.keys())
        environmentStorage.push_back((key + QLatin1Char('=') + environment.value(key)).toUtf8());
    std::vector<char*> environmentPointers;
    environmentPointers.reserve(environmentStorage.size() + 1U);
    for (QByteArray& entry : environmentStorage)
        environmentPointers.push_back(entry.data());
    environmentPointers.push_back(nullptr);
    const QByteArray workingDirectory = _config.effectiveWorkingDirectory().toUtf8();

    // 创建 PTY 并设置初始窗口大小
    winsize size{};
    size.ws_col = static_cast<unsigned short>(_columns);
    size.ws_row = static_cast<unsigned short>(_rows);
    int slaveFd = -1;
    if (::openpty(&_masterFd, &slaveFd, nullptr, nullptr, &size) != 0) {
        failStart(QStringLiteral("openpty() failed: %1").arg(errnoMessage(errno)));
        return;
    }
    // master 设为非阻塞且 close-on-exec，避免泄漏给 execve 后的目标程序
    if (!setDescriptorFlag(_masterFd, F_GETFL, O_NONBLOCK)
        || !setDescriptorFlag(_masterFd, F_GETFD, FD_CLOEXEC)) {
        const int error = errno;
        ::close(slaveFd);
        closeDescriptors();
        failStart(QStringLiteral("Cannot configure PTY master: %1").arg(errnoMessage(error)));
        return;
    }

    // 错误管道：子进程 execve 失败时通过它回传 errno 给父进程
    int errorPipe[2] = {-1, -1};
    if (::pipe2(errorPipe, O_CLOEXEC) != 0) {
        const int error = errno;
        ::close(slaveFd);
        closeDescriptors();
        failStart(QStringLiteral("pipe2() failed: %1").arg(errnoMessage(error)));
        return;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        // ===== 子进程分支：仅使用 async-signal-safe 的函数 =====
        ::close(errorPipe[0]);
        ::close(_masterFd);
        // setsid 脱离父进程控制终端；TIOCSCTTY 将 slave 设为控制终端
        // dup2 将 slave 重定向到 stdin/stdout/stderr
        if (::setsid() < 0 || ::ioctl(slaveFd, TIOCSCTTY, 0) < 0
            || ::dup2(slaveFd, STDIN_FILENO) < 0
            || ::dup2(slaveFd, STDOUT_FILENO) < 0
            || ::dup2(slaveFd, STDERR_FILENO) < 0) {
            childFailure(errorPipe[1], errno);
        }
        if (slaveFd > STDERR_FILENO)
            ::close(slaveFd);
        if (!workingDirectory.isEmpty()
            && ::chdir(workingDirectory.constData()) != 0) {
            childFailure(errorPipe[1], errno);
        }
        // execve 成功后此进程镜像被替换；失败则通过管道回传 errno
        ::execve(argumentStorage[0].constData(), arguments.data(),
                 environmentPointers.data());
        childFailure(errorPipe[1], errno);
    }

    // ===== 父进程分支 =====
    ::close(slaveFd);
    ::close(errorPipe[1]);
    if (child < 0) {
        const int error = errno;
        ::close(errorPipe[0]);
        closeDescriptors();
        failStart(QStringLiteral("fork() failed: %1").arg(errnoMessage(error)));
        return;
    }
    _childPid = child;

    // 阻塞读取错误管道：读到 0 字节表示子进程 execve 成功（管道被 close 写端关闭）；
    // 读到 4 字节表示失败，内容为子进程写入的 errno
    int childError = 0;
    ssize_t result;
    do {
        result = ::read(errorPipe[0], &childError, sizeof(childError));
    } while (result < 0 && errno == EINTR);
    const int startupReadError = result < 0 ? errno : 0;
    ::close(errorPipe[0]);
    if (result < 0) {
        // 读取本身失败：杀掉子进程并回收
        ::kill(child, SIGKILL);
        ::kill(-child, SIGKILL);
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        _childPid = -1;
        closeDescriptors();
        failStart(QStringLiteral("Cannot read child startup status: %1")
                      .arg(errnoMessage(startupReadError)));
        return;
    }
    if (result > 0) {
        // 子进程 execve 失败：回收并报告错误码
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        _childPid = -1;
        closeDescriptors();
        failStart(QStringLiteral("Cannot launch '%1': %2")
                      .arg(executable, errnoMessage(childError)));
        return;
    }

    // 启动成功：建立读/写通知与退出轮询定时器
    _readNotifier = new QSocketNotifier(_masterFd, QSocketNotifier::Read, this);
    connect(_readNotifier, &QSocketNotifier::activated,
            this, &PtySession::drainOutput);
    _writeNotifier = new QSocketNotifier(_masterFd, QSocketNotifier::Write, this);
    _writeNotifier->setEnabled(false);
    connect(_writeNotifier, &QSocketNotifier::activated,
            this, &PtySession::flushInput);
    _exitTimer = new QTimer(this);
    _exitTimer->setInterval(25);
    connect(_exitTimer, &QTimer::timeout, this, &PtySession::checkChildExit);
    _exitTimer->start();
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        _acceptingInput = true;
    }
    transition(State::Running);
    emit started();
}

// 线程安全地将输入入队：在互斥锁保护下追加到队列并更新字节数
// 通过 invokeMethod 切换到主线程异步触发 flushInput 写出
bool PtySession::tryEnqueueInput(const QByteArray& data)
{
    if (data.isEmpty())
        return true;
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        if (!_acceptingInput || static_cast<std::size_t>(data.size()) > InputCapacity - _inputBytes)
            return false;
        _inputQueue.push_back(data);
        _inputBytes += static_cast<std::size_t>(data.size());
    }
    QMetaObject::invokeMethod(this, &PtySession::flushInput, Qt::QueuedConnection);
    return true;
}

// 主线程槽函数：将队列中的输入写入 master fd
// 采用部分写入（_inputOffset 记录当前块进度）；遇到 EAGAIN 时启用写通知等待下次可写
void PtySession::flushInput()
{
    if (_masterFd < 0 || !_writeNotifier)
        return;
    std::unique_lock<std::mutex> lock(_inputMutex);
    while (!_inputQueue.empty()) {
        QByteArray& front = _inputQueue.front();
        const char* data = front.constData() + _inputOffset;
        const auto remaining = static_cast<std::size_t>(front.size() - _inputOffset);
        const ssize_t written = ::write(_masterFd, data, remaining);
        if (written > 0) {
            _inputOffset += static_cast<qsizetype>(written);
            _inputBytes -= static_cast<std::size_t>(written);
            if (_inputOffset == front.size()) {
                _inputQueue.pop_front();
                _inputOffset = 0;
            }
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        // 写缓冲区满：开启写通知，等下次可写事件再继续
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            _writeNotifier->setEnabled(true);
            return;
        }
        // 其他写入错误：上报并关闭
        const int error = written < 0 ? errno : EIO;
        _writeNotifier->setEnabled(false);
        lock.unlock();
        reportIoError(QStringLiteral("PTY write"), error);
        return;
    }
    _writeNotifier->setEnabled(false);
}

// 读通知槽函数：循环读取 master 输出并通过 dataReady 信号投递
// EIO 通常表示子进程已关闭 slave 端，视为正常结束（由退出轮询处理）
void PtySession::drainOutput()
{
    if (_masterFd < 0 || _readPaused)
        return;
    std::array<char, ReadBufferSize> buffer{};
    for (;;) {
        const ssize_t count = ::read(_masterFd, buffer.data(), buffer.size());
        if (count > 0) {
            emit dataReady(QByteArray(buffer.data(), static_cast<qsizetype>(count)));
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO))
            return;
        if (count == 0)
            return;
        reportIoError(QStringLiteral("PTY read"), errno);
        return;
    }
}

// 调整 PTY 窗口大小：通过 TIOCSWINSZ ioctl 同步给内核，子进程会收到 SIGWINCH
void PtySession::resize(int columns, int rows)
{
    if (columns <= 0 || rows <= 0 || columns > USHRT_MAX || rows > USHRT_MAX)
        return;
    _columns = columns;
    _rows = rows;
    if (_masterFd < 0)
        return;
    winsize size{};
    size.ws_col = static_cast<unsigned short>(columns);
    size.ws_row = static_cast<unsigned short>(rows);
    if (::ioctl(_masterFd, TIOCSWINSZ, &size) != 0)
        reportIoError(QStringLiteral("PTY resize"), errno);
}

// 暂停/恢复读取：通过禁用读通知实现背压；恢复时主动尝试 drain 一次
void PtySession::setReadPaused(bool paused)
{
    _readPaused = paused;
    if (_readNotifier)
        _readNotifier->setEnabled(!paused);
    if (!paused)
        drainOutput();
}

// 用户主动关闭：清空输入队列，向进程组发送 SIGHUP 通知子进程退出
// 若无子进程则直接发 exited 信号并 finalize
void PtySession::requestClose()
{
    if (_state == State::Closed || _state == State::Closing)
        return;
    _userCloseRequested = true;
    transition(State::Closing);
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        _acceptingInput = false;
        _inputQueue.clear();
        _inputBytes = 0;
        _inputOffset = 0;
    }
    if (_writeNotifier)
        _writeNotifier->setEnabled(false);
    if (_childPid > 0) {
        ::kill(-static_cast<pid_t>(_childPid), SIGHUP);
        checkChildExit();
    } else {
        if (!_exitEmitted) {
            _exitEmitted = true;
            emit exited(UnknownExitCode, TransportExitReason::UserClosed);
        }
        finalizeClose();
    }
}

// 25ms 轮询：WNOHANG 非阻塞回收子进程
// 关闭流程中的渐进式强制终止：1s 后 SIGTERM，1.5s 后 SIGKILL（40/60 次 × 25ms）
void PtySession::checkChildExit()
{
    if (_childPid <= 0)
        return;
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(_childPid), &status, WNOHANG);
    if (result == static_cast<pid_t>(_childPid)) {
        // 子进程已退出：先 drain 残留输出，再解析状态码
        _childPid = -1;
        drainOutput();
        finish(status);
        finalizeClose();
        return;
    }
    if (result < 0 && errno != EINTR) {
        const int error = errno;
        _childPid = -1;
        reportIoError(QStringLiteral("waitpid"), error);
        finalizeClose();
        return;
    }
    if (_state != State::Closing)
        return;
    // 关闭中渐进升级信号强度
    ++_closePolls;
    if (_closePolls == 40)
        ::kill(-static_cast<pid_t>(_childPid), SIGTERM);
    else if (_closePolls >= 60)
        ::kill(-static_cast<pid_t>(_childPid), SIGKILL);
}

// 解析 waitpid 状态并发射 exited 信号：区分正常退出/信号终止/用户关闭
// 信号终止的退出码遵循 shell 惯例 128+signo，便于上层透传给脚本
void PtySession::finish(int status)
{
    if (_exitEmitted)
        return;
    _exitEmitted = true;
    quint32 code = UnknownExitCode;
    TransportExitReason reason = TransportExitReason::Crash;
    if (_userCloseRequested) {
        reason = TransportExitReason::UserClosed;
        if (WIFEXITED(status))
            code = static_cast<quint32>(WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            code = static_cast<quint32>(128 + WTERMSIG(status));
    } else if (WIFEXITED(status)) {
        code = static_cast<quint32>(WEXITSTATUS(status));
        reason = code == 0 ? TransportExitReason::NormalExit
                           : TransportExitReason::FailedExit;
    } else if (WIFSIGNALED(status)) {
        code = static_cast<quint32>(128 + WTERMSIG(status));
        reason = TransportExitReason::Crash;
    }
    emit exited(code, reason);
}

// 上报 I/O 错误并触发关闭：先发 errorOccurred，再补发 exited（若尚未发过）
void PtySession::reportIoError(const QString& operation, int error)
{
    if (_state == State::Closed)
        return;
    emit errorOccurred(QStringLiteral("%1 failed: %2")
                           .arg(operation, errnoMessage(error)));
    if (!_exitEmitted) {
        _exitEmitted = true;
        emit exited(UnknownExitCode, _userCloseRequested
                        ? TransportExitReason::UserClosed
                        : TransportExitReason::IoError);
    }
    requestClose();
}

// 启动失败专用：报告错误后直接进入终态
void PtySession::failStart(const QString& error)
{
    emit errorOccurred(error);
    _exitEmitted = true;
    emit exited(UnknownExitCode, TransportExitReason::StartFailed);
    finalizeClose();
}

// 关闭读/写通知并释放 master fd
void PtySession::closeDescriptors()
{
    if (_readNotifier)
        _readNotifier->setEnabled(false);
    if (_writeNotifier)
        _writeNotifier->setEnabled(false);
    if (_masterFd >= 0) {
        ::close(_masterFd);
        _masterFd = -1;
    }
}

// 终态收尾：停定时器、关描述符、清输入队列，并切换状态、发射 closed 信号
void PtySession::finalizeClose()
{
    if (_state == State::Closed)
        return;
    if (_exitTimer)
        _exitTimer->stop();
    closeDescriptors();
    {
        std::lock_guard<std::mutex> lock(_inputMutex);
        _acceptingInput = false;
        _inputQueue.clear();
        _inputBytes = 0;
        _inputOffset = 0;
    }
    transition(State::Closed);
    emit closed();
}

} // namespace NovaTerm::Linux
