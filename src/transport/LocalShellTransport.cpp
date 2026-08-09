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
//  Linux 实现 — 独立 PTY 会话后端
// ═══════════════════════════════════════════════════════════════════

#ifndef _WIN32

#include "platform/linux/pty/PtySession.h"

using NovaTerm::Linux::PtySession;

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
        if (shell.isEmpty())
            shell = LocalShellProfiles::platformDefault().executable;
        _config.profile.name = shell;
        _config.profile.executable = shell;
        _config.profile.arguments = _shellArgs;
        _config.workingDirectory = _workingDir;
    }

    _errorString.clear();
    _readPaused.store(false, std::memory_order_release);
    setLifecycleState(LifecycleState::Starting);
    auto* session = new PtySession(_config, _cols, _rows, this);
    _linuxSession = session;
    const quint64 generation = ++_linuxGeneration;

    connect(session, &PtySession::started, this, [this, session, generation] {
        if (_linuxGeneration != generation || _linuxSession != session)
            return;
        _connected = true;
        setLifecycleState(LifecycleState::Running);
        emit connected();
    });
    connect(session, &PtySession::dataReady, this,
            [this, session, generation](const QByteArray& data) {
        if (_linuxGeneration == generation && _linuxSession == session)
            emit readyRead(data);
    });
    connect(session, &PtySession::errorOccurred, this,
            [this, session, generation](const QString& error) {
        if (_linuxGeneration != generation || _linuxSession != session)
            return;
        _errorString = error;
        emit errorOccurred(error);
    });
    connect(session, &PtySession::exited, this,
            [this, session, generation](quint32 code, TransportExitReason reason) {
        if (_linuxGeneration == generation && _linuxSession == session)
            emit exited(code, reason);
    });
    connect(session, &PtySession::closed, this, [this, session, generation] {
        if (_linuxGeneration != generation || _linuxSession != session)
            return;
        _connected = false;
        _linuxSession = nullptr;
        setLifecycleState(LifecycleState::Closed);
        emit disconnected();
        session->deleteLater();
    });
    QMetaObject::invokeMethod(session, &PtySession::start, Qt::QueuedConnection);
    return true;
}

void LocalShellTransport::disconnect()
{
    if (_state == LifecycleState::Idle || _state == LifecycleState::Closed
        || _state == LifecycleState::Closing)
        return;
    _connected = false;
    _readPaused.store(false, std::memory_order_release);
    setLifecycleState(LifecycleState::Closing);
    if (_linuxSession) {
        QMetaObject::invokeMethod(_linuxSession.data(), &PtySession::requestClose,
                                  Qt::QueuedConnection);
    }
}

bool LocalShellTransport::setReadPaused(bool paused)
{
    _readPaused.store(paused, std::memory_order_release);
    if (_linuxSession)
        _linuxSession->setReadPaused(paused);
    return true;
}

void LocalShellTransport::write(const QByteArray& data)
{
    if (!_linuxSession || _state != LifecycleState::Running || data.isEmpty())
        return;
    if (!_linuxSession->tryEnqueueInput(data)) {
        _errorString = QStringLiteral("PTY input queue capacity exceeded or closed");
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

    if (_linuxSession)
        _linuxSession->resize(cols, rows);
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
