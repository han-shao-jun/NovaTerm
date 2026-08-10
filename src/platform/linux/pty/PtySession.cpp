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

constexpr quint32 UnknownExitCode = (std::numeric_limits<quint32>::max)();

QString errnoMessage(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}

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

bool setDescriptorFlag(int fd, int command, int flag)
{
    const int current = ::fcntl(fd, command, 0);
    if (current < 0)
        return false;
    const int setCommand = command == F_GETFL ? F_SETFL : F_SETFD;
    return ::fcntl(fd, setCommand, current | flag) == 0;
}

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

PtySession::~PtySession()
{
    closeDescriptors();
    if (_childPid > 0) {
        ::kill(-static_cast<pid_t>(_childPid), SIGKILL);
        int status = 0;
        while (::waitpid(static_cast<pid_t>(_childPid), &status, 0) < 0
               && errno == EINTR) {
        }
    }
}

void PtySession::transition(State state)
{
    if (_state == state)
        return;
    _state = state;
    emit stateChanged(state);
}

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

    winsize size{};
    size.ws_col = static_cast<unsigned short>(_columns);
    size.ws_row = static_cast<unsigned short>(_rows);
    int slaveFd = -1;
    if (::openpty(&_masterFd, &slaveFd, nullptr, nullptr, &size) != 0) {
        failStart(QStringLiteral("openpty() failed: %1").arg(errnoMessage(errno)));
        return;
    }
    if (!setDescriptorFlag(_masterFd, F_GETFL, O_NONBLOCK)
        || !setDescriptorFlag(_masterFd, F_GETFD, FD_CLOEXEC)) {
        const int error = errno;
        ::close(slaveFd);
        closeDescriptors();
        failStart(QStringLiteral("Cannot configure PTY master: %1").arg(errnoMessage(error)));
        return;
    }

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
        ::close(errorPipe[0]);
        ::close(_masterFd);
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
        ::execve(argumentStorage[0].constData(), arguments.data(),
                 environmentPointers.data());
        childFailure(errorPipe[1], errno);
    }

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

    int childError = 0;
    ssize_t result;
    do {
        result = ::read(errorPipe[0], &childError, sizeof(childError));
    } while (result < 0 && errno == EINTR);
    const int startupReadError = result < 0 ? errno : 0;
    ::close(errorPipe[0]);
    if (result < 0) {
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
        int status = 0;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        _childPid = -1;
        closeDescriptors();
        failStart(QStringLiteral("Cannot launch '%1': %2")
                      .arg(executable, errnoMessage(childError)));
        return;
    }

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
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            _writeNotifier->setEnabled(true);
            return;
        }
        const int error = written < 0 ? errno : EIO;
        _writeNotifier->setEnabled(false);
        lock.unlock();
        reportIoError(QStringLiteral("PTY write"), error);
        return;
    }
    _writeNotifier->setEnabled(false);
}

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

void PtySession::setReadPaused(bool paused)
{
    _readPaused = paused;
    if (_readNotifier)
        _readNotifier->setEnabled(!paused);
    if (!paused)
        drainOutput();
}

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

void PtySession::checkChildExit()
{
    if (_childPid <= 0)
        return;
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(_childPid), &status, WNOHANG);
    if (result == static_cast<pid_t>(_childPid)) {
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
    ++_closePolls;
    if (_closePolls == 40)
        ::kill(-static_cast<pid_t>(_childPid), SIGTERM);
    else if (_closePolls >= 60)
        ::kill(-static_cast<pid_t>(_childPid), SIGKILL);
}

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

void PtySession::failStart(const QString& error)
{
    emit errorOccurred(error);
    _exitEmitted = true;
    emit exited(UnknownExitCode, TransportExitReason::StartFailed);
    finalizeClose();
}

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
