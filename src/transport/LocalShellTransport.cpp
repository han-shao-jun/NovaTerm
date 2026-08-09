#include "LocalShellTransport.h"

#include <QDebug>
#include <QProcessEnvironment>

// ═══════════════════════════════════════════════════════════════════
//  跨平台公共部分
// ═══════════════════════════════════════════════════════════════════

LocalShellTransport::LocalShellTransport(QObject* parent)
    : ITransport(parent)
{
}

LocalShellTransport::~LocalShellTransport()
{
    disconnect();
}

void LocalShellTransport::setShellProgram(const QString& program)
{
    _shellProgram = program;
    _config.profile.executable = program;
    if (_config.profile.name.isEmpty())
        _config.profile.name = program;
}

void LocalShellTransport::setShellArgs(const QStringList& args)
{
    _shellArgs = args;
    _config.profile.arguments = args;
}

void LocalShellTransport::setWorkingDirectory(const QString& dir)
{
    _workingDir = dir;
    _config.workingDirectory = dir;
}

void LocalShellTransport::setEnvironment(const QStringList& env)
{
    _environment = env;
    QProcessEnvironment environment;
    for (const QString& entry : env) {
        const qsizetype equals = entry.indexOf(QLatin1Char('='));
        if (equals > 0)
            environment.insert(entry.left(equals), entry.mid(equals + 1));
    }
    _config.environment = environment;
}

void LocalShellTransport::setShellProfile(const LocalShellProfile& profile)
{
    _config.profile = profile;
    _shellProgram = profile.executable;
    _shellArgs = profile.arguments;
    _workingDir = profile.workingDirectory;
}

void LocalShellTransport::setSessionConfig(const LocalShellConfig& config)
{
    _config = config;
    _shellProgram = config.profile.executable;
    _shellArgs = config.profile.arguments;
    _workingDir = config.effectiveWorkingDirectory();
}

void LocalShellTransport::setLifecycleState(LifecycleState state)
{
    if (_state == state)
        return;
    _state = state;
    emit lifecycleStateChanged(state);
}

bool LocalShellTransport::isConnected() const
{
    return _connected;
}

QString LocalShellTransport::errorString() const
{
    return _errorString;
}

// ═══════════════════════════════════════════════════════════════════
//  Unix 实现 — posix_openpt() + fork() + QSocketNotifier
// ═══════════════════════════════════════════════════════════════════

#ifndef _WIN32

#include <QCoreApplication>
#include <QSocketNotifier>
#include <QTimer>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

bool LocalShellTransport::connectToHost()
{
    if (_connected)
        disconnect();
    _readPaused.store(false, std::memory_order_release);

    // 确定要启动的 shell
    QString shell = _shellProgram;
    if (shell.isEmpty()) {
        shell = QString::fromLocal8Bit(qgetenv("SHELL"));
        if (shell.isEmpty())
            shell = QStringLiteral("/bin/bash");
    }

    // ── 1. 打开 PTY ────────────────────────────────────────
    // openpty() 同时返回 master/slave fd 并自动处理
    // grantpt/unlockpt/ptsname 等细节
    int slaveFd;
    if (openpty(&_masterFd, &slaveFd, nullptr, nullptr, nullptr) != 0) {
        _errorString = QStringLiteral("openpty() failed: ")
                       + QString::fromLocal8Bit(strerror(errno));
        qWarning() << "LocalShellTransport:" << _errorString;
        return false;
    }

    // 设置 master 端为非阻塞模式，避免 QSocketNotifier 饥饿主事件循环
    int flags = fcntl(_masterFd, F_GETFL, 0);
    fcntl(_masterFd, F_SETFL, flags | O_NONBLOCK);

    // ── 2. fork 子进程 ──────────────────────────────────────
    _childPid = fork();
    if (_childPid == -1) {
        _errorString = QStringLiteral("fork() failed: ")
                       + QString::fromLocal8Bit(strerror(errno));
        qWarning() << "LocalShellTransport:" << _errorString;
        close(_masterFd);
        close(slaveFd);
        _masterFd = -1;
        return false;
    }

    if (_childPid == 0) {
        // ── 子进程 ──────────────────────────────────────────
        close(_masterFd);

        // 创建新会话，使子进程成为控制终端会话的首进程
        setsid();

        // 将 slave PTY 设为本进程的控制终端
        if (ioctl(slaveFd, TIOCSCTTY, 0) != 0) {
            _exit(127);
        }

        // 复制 slave fd 到 stdin/stdout/stderr
        dup2(slaveFd, STDIN_FILENO);
        dup2(slaveFd, STDOUT_FILENO);
        dup2(slaveFd, STDERR_FILENO);
        if (slaveFd > STDERR_FILENO)
            close(slaveFd);

        // 设置工作目录
        if (!_workingDir.isEmpty())
            ::chdir(_workingDir.toLocal8Bit().constData());

        // 设置环境变量
        for (const QString& env : _environment) {
            QByteArray ba = env.toLocal8Bit();
            char* str = strdup(ba.constData());
            putenv(str);  // intentionally leaked in child (forked)
        }

        // 构建 argv
        QByteArray prog = shell.toLocal8Bit();
        char** argv = new char*[_shellArgs.size() + 2];
        argv[0] = strdup(prog.constData());
        for (int i = 0; i < _shellArgs.size(); ++i) {
            QByteArray arg = _shellArgs[i].toLocal8Bit();
            argv[i + 1] = strdup(arg.constData());
        }
        argv[_shellArgs.size() + 1] = nullptr;

        execvp(argv[0], argv);
        _exit(127);  // exec 失败
    }

    // ── 3. 父进程：设置异步读取 ────────────────────────────
    close(slaveFd);

    _notifier = new QSocketNotifier(_masterFd, QSocketNotifier::Read, this);
    QObject::connect(_notifier, &QSocketNotifier::activated,
                     this, [this](int /*fd*/) {
        if (_readPaused.load(std::memory_order_acquire))
            return;
        char buf[4096];
        for (;;) {
            ssize_t n = ::read(_masterFd, buf, sizeof(buf));
            if (n > 0) {
                emit readyRead(QByteArray(buf, static_cast<int>(n)));
            } else if (n == 0) {
                // EOF — 子进程已关闭 PTY
                disconnect();
                return;
            } else {
                // n < 0
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;  // 非阻塞模式下无更多数据
                if (errno == EIO) {
                    // 子进程已退出
                    disconnect();
                    return;
                }
                break;
            }
        }
    });

    // ── 4. 子进程退出检测 ──────────────────────────────────
    _exitTimer = new QTimer(this);
    _exitTimer->setInterval(200);
    QObject::connect(_exitTimer, &QTimer::timeout,
                     this, &LocalShellTransport::checkChildExit);
    _exitTimer->start();

    _connected = true;
    emit connected();
    return true;
}

void LocalShellTransport::checkChildExit()
{
    if (_childPid <= 0)
        return;

    int status;
    pid_t result = waitpid(_childPid, &status, WNOHANG);
    if (result == _childPid) {
        _childPid = -1;
        _connected = false;
        if (_exitTimer) {
            _exitTimer->stop();
        }
        emit disconnected();
    } else if (result < 0 && errno != EINTR) {
        // waitpid 出错（非中断）
        _childPid = -1;
        _connected = false;
        if (_exitTimer) {
            _exitTimer->stop();
        }
        emit disconnected();
    }
}

void LocalShellTransport::disconnect()
{
    if (!_connected && _masterFd == -1)
        return;

    _connected = false;

    if (_exitTimer) {
        _exitTimer->stop();
        delete _exitTimer;
        _exitTimer = nullptr;
    }

    if (_notifier) {
        delete _notifier;
        _notifier = nullptr;
    }

    if (_masterFd != -1) {
        close(_masterFd);
        _masterFd = -1;
    }

    if (_childPid > 0) {
        // 发送 SIGHUP 通知子进程终端已关闭
        kill(_childPid, SIGHUP);
        // 等待最多 2 秒让子进程优雅退出
        int status;
        for (int i = 0; i < 20; ++i) {
            if (waitpid(_childPid, &status, WNOHANG) == _childPid)
                break;
            usleep(100000);  // 100ms
        }
        // 若仍未退出则强制终止
        if (waitpid(_childPid, &status, WNOHANG) != _childPid) {
            kill(_childPid, SIGKILL);
            waitpid(_childPid, &status, 0);
        }
        _childPid = -1;
    }

    emit disconnected();
}

bool LocalShellTransport::setReadPaused(bool paused)
{
    _readPaused.store(paused, std::memory_order_release);
    if (_notifier)
        _notifier->setEnabled(!paused);
    return true;
}

void LocalShellTransport::write(const QByteArray& data)
{
    if (_masterFd != -1) {
        const auto written = ::write(
            _masterFd, data.constData(), static_cast<size_t>(data.size()));
        if (written > 0)
            emit bytesWritten(written);
    }
}

void LocalShellTransport::resizeTerminal(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    _cols = cols;
    _rows = rows;

    if (_masterFd != -1) {
        struct winsize ws;
        ws.ws_row = static_cast<unsigned short>(rows);
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(_masterFd, TIOCSWINSZ, &ws);
    }

    // 通知子进程（通过 SIGWINCH）
    if (_childPid > 0)
        kill(_childPid, SIGWINCH);
}

#endif // !_WIN32

// ═══════════════════════════════════════════════════════════════════
//  Windows 实现 — CreatePseudoConsole (ConPTY)
//  代码从原 WinConPty 迁移而来，适配 ITransport 接口
// ═══════════════════════════════════════════════════════════════════

#ifdef _WIN32

#include "platform/windows/conpty/ConPtySession.h"

#include <QMetaObject>
#include <QThread>

using NovaTerm::Windows::ConPtySession;

struct LocalShellTransport::ResizeDispatchState
{
    std::mutex mutex;
    int columns{80};
    int rows{24};
    bool pending{false};
};

// ── ITransport 实现 ──────────────────────────────────────────────

bool LocalShellTransport::connectToHost()
{
    if (_state == LifecycleState::Starting
        || _state == LifecycleState::Running
        || _state == LifecycleState::Closing) {
        _errorString = QStringLiteral("Local shell session is already active or closing");
        emit errorOccurred(_errorString);
        return false;
    }
    if (!_config.isValid()) {
        QString shell = _shellProgram;
        if (shell.isEmpty()) {
            shell = QString::fromLocal8Bit(qgetenv("ComSpec"));
            if (shell.isEmpty())
                shell = QStringLiteral("cmd.exe");
        }
        _config.profile.name = shell;
        _config.profile.executable = shell;
        _config.profile.arguments = _shellArgs;
        _config.workingDirectory = _workingDir;
    }

    _errorString.clear();
    _readPaused.store(false, std::memory_order_release);
    setLifecycleState(LifecycleState::Starting);

    auto* thread = new QThread;
    thread->setObjectName(QStringLiteral("NovaTerm ConPTY Lifecycle"));
    auto* session = new ConPtySession(_config, _cols, _rows);
    session->moveToThread(thread);
    _windowsThread = thread;
    _windowsSession = session;
    _resizeDispatch = std::make_shared<ResizeDispatchState>();
    const quint64 generation = ++_windowsGeneration;

    QObject::connect(session, &ConPtySession::started, this,
                     [this, session, generation] {
        if (_windowsGeneration != generation || _windowsSession != session)
            return;
        _connected = true;
        setLifecycleState(LifecycleState::Running);
        emit connected();
    });
    const QPointer<ConPtySession> sessionGuard(session);
    QObject::connect(session, &ConPtySession::dataReady, this,
                     [this, sessionGuard, generation](const QByteArray& data) {
        if (!sessionGuard || _windowsGeneration != generation
            || _windowsSession != sessionGuard)
            return;
        const QPointer<LocalShellTransport> self(this);
        emit readyRead(data);
        if (self && sessionGuard)
            sessionGuard->acknowledgeOutput();
    });
    QObject::connect(session, &ConPtySession::errorOccurred, this,
                     [this, session, generation](const QString& error) {
        if (_windowsGeneration != generation || _windowsSession != session)
            return;
        _errorString = error;
        emit errorOccurred(error);
    });
    QObject::connect(session, &ConPtySession::exited, this,
                     [this, session, generation](quint32 exitCode, TransportExitReason reason) {
        if (_windowsGeneration == generation && _windowsSession == session)
            emit exited(exitCode, reason);
    });
    QObject::connect(session, &ConPtySession::closed, this,
                     [this, generation] {
        if (_windowsGeneration == generation) {
            _connected = false;
            _windowsSession = nullptr;
            _windowsThread = nullptr;
            setLifecycleState(LifecycleState::Closed);
            emit disconnected();
        }
    });
    QObject::connect(session, &ConPtySession::closed,
                     thread, &QThread::quit, Qt::QueuedConnection);
    QObject::connect(thread, &QThread::finished,
                     session, &QObject::deleteLater);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    QMetaObject::invokeMethod(session, &ConPtySession::start,
                              Qt::QueuedConnection);
    return true;
}

void LocalShellTransport::disconnect()
{
    if (_state == LifecycleState::Idle || _state == LifecycleState::Closed
        || _state == LifecycleState::Closing) {
        return;
    }
    _connected = false;
    _readPaused.store(false, std::memory_order_release);
    setLifecycleState(LifecycleState::Closing);
    if (_windowsSession) {
        QMetaObject::invokeMethod(_windowsSession.data(), &ConPtySession::requestClose,
                                  Qt::QueuedConnection);
    }
}

bool LocalShellTransport::setReadPaused(bool paused)
{
    _readPaused.store(paused, std::memory_order_release);
    if (_windowsSession) {
        ConPtySession* session = _windowsSession.data();
        QMetaObject::invokeMethod(
            session,
            [session, paused] { session->setReadPaused(paused); },
            Qt::QueuedConnection);
    }
    return true;
}

void LocalShellTransport::write(const QByteArray& data)
{
    if (!_windowsSession || _state != LifecycleState::Running || data.isEmpty())
        return;
    if (!_windowsSession->tryEnqueueInput(data)) {
        _errorString = QStringLiteral("ConPTY input queue capacity exceeded or closed");
        emit errorOccurred(_errorString);
    } else {
        emit bytesWritten(data.size());
    }
}

void LocalShellTransport::resizeTerminal(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    _cols = cols;
    _rows = rows;
    if (_windowsSession) {
        const auto dispatch = _resizeDispatch;
        bool post = false;
        {
            std::lock_guard<std::mutex> lock(dispatch->mutex);
            dispatch->columns = cols;
            dispatch->rows = rows;
            if (!dispatch->pending) {
                dispatch->pending = true;
                post = true;
            }
        }
        if (!post)
            return;
        ConPtySession* session = _windowsSession.data();
        QMetaObject::invokeMethod(
            session,
            [session, dispatch] {
                int latestColumns = 0;
                int latestRows = 0;
                {
                    std::lock_guard<std::mutex> lock(dispatch->mutex);
                    latestColumns = dispatch->columns;
                    latestRows = dispatch->rows;
                    dispatch->pending = false;
                }
                session->resize(latestColumns, latestRows);
            },
            Qt::QueuedConnection);
    }
}

#endif // _WIN32
