#include "LocalShellTransport.h"

#include <QDebug>

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
}

void LocalShellTransport::setShellArgs(const QStringList& args)
{
    _shellArgs = args;
}

void LocalShellTransport::setWorkingDirectory(const QString& dir)
{
    _workingDir = dir;
}

void LocalShellTransport::setEnvironment(const QStringList& env)
{
    _environment = env;
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

void LocalShellTransport::write(const QByteArray& data)
{
    if (_masterFd != -1) {
        ::write(_masterFd, data.constData(), static_cast<size_t>(data.size()));
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

#include <QCoreApplication>
#include <sstream>

// ── Win32 helpers ────────────────────────────────────────────────

static inline QString winError(DWORD err)
{
    wchar_t buf[256];
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, 256, nullptr);
    return QString::fromWCharArray(buf).trimmed();
}

static inline QString lastWinError()
{
    return winError(GetLastError());
}

// Dynamically resolve ConPTY functions (available Windows 10 1809+)
using CreatePseudoConsoleFn = HRESULT(WINAPI *)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
using ResizePseudoConsoleFn  = HRESULT(WINAPI *)(HPCON, COORD);
using ClosePseudoConsoleFn   = VOID(WINAPI *)(HPCON);

static CreatePseudoConsoleFn pCreatePseudoConsole = nullptr;
static ResizePseudoConsoleFn  pResizePseudoConsole  = nullptr;
static ClosePseudoConsoleFn   pClosePseudoConsole   = nullptr;

static bool resolveConPty()
{
    if (pCreatePseudoConsole)
        return true;
    HMODULE h = LoadLibraryW(L"kernel32.dll");
    if (!h) return false;
    pCreatePseudoConsole = reinterpret_cast<CreatePseudoConsoleFn>(
        GetProcAddress(h, "CreatePseudoConsole"));
    pResizePseudoConsole = reinterpret_cast<ResizePseudoConsoleFn>(
        GetProcAddress(h, "ResizePseudoConsole"));
    pClosePseudoConsole = reinterpret_cast<ClosePseudoConsoleFn>(
        GetProcAddress(h, "ClosePseudoConsole"));
    return pCreatePseudoConsole && pResizePseudoConsole && pClosePseudoConsole;
}

// ── ITransport 实现 ──────────────────────────────────────────────

bool LocalShellTransport::connectToHost()
{
    if (_connected)
        disconnect();

    if (!resolveConPty()) {
        _errorString = QStringLiteral("ConPTY not available (need Windows 10 1809+)");
        qWarning() << "LocalShellTransport:" << _errorString;
        return false;
    }

    if (_running.load())
        disconnect();

    if (!createPipes()) return false;
    if (!createConPty(_cols, _rows)) return false;

    // 构建命令行
    QString shell = _shellProgram;
    if (shell.isEmpty()) {
        shell = QString::fromLocal8Bit(qgetenv("ComSpec"));
        if (shell.isEmpty())
            shell = QStringLiteral("cmd.exe");
    }

    std::wstringstream cmdLine;
    cmdLine << L"\"" << shell.toStdWString() << L"\"";
    for (const QString& a : _shellArgs)
        cmdLine << L" " << a.toStdWString();

    if (!launchProcess(QString::fromStdWString(cmdLine.str()))) return false;

    // 启动 reader 线程
    _running.store(true);
    _readerThread = QThread::create([this]() {
        char buf[4096];
        DWORD n;
        while (_running.load() &&
               ReadFile(_hOutputRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            QByteArray data(buf, static_cast<int>(n));
            QMetaObject::invokeMethod(this, [this, d = std::move(data)]() {
                if (_running.load())
                    emit readyRead(d);   // ITransport 信号
            }, Qt::QueuedConnection);
        }
        // ReadFile 返回失败或 0 字节 — shell 已退出
        if (_running.load()) {
            QMetaObject::invokeMethod(this, [this]() {
                disconnect();
            }, Qt::QueuedConnection);
        }
    });
    _readerThread->start();

    _connected = true;
    emit connected();
    return true;
}

void LocalShellTransport::disconnect()
{
    _running.store(false);
    _connected = false;

    // 移除待处理的事件
    QCoreApplication::removePostedEvents(this);

    // 关闭 ConPTY — 强制 reader 线程的 ReadFile 返回
    if (_hPC) {
        pClosePseudoConsole(_hPC);
        _hPC = nullptr;
    }

    // 终止进程
    if (_hProcess) {
        TerminateProcess(_hProcess, 0);
        WaitForSingleObject(_hProcess, 2000);
        CloseHandle(_hProcess);
        _hProcess = nullptr;
    }
    if (_hThread) {
        CloseHandle(_hThread);
        _hThread = nullptr;
    }

    // 等待 reader 线程
    if (_readerThread) {
        _readerThread->wait(3000);
        delete _readerThread;
        _readerThread = nullptr;
    }

    // 关闭管道句柄
    if (_hInputWrite)  { CloseHandle(_hInputWrite);  _hInputWrite  = nullptr; }
    if (_hInputPipe)   { CloseHandle(_hInputPipe);   _hInputPipe   = nullptr; }
    if (_hOutputRead)  { CloseHandle(_hOutputRead);  _hOutputRead  = nullptr; }
    if (_hOutputWrite) { CloseHandle(_hOutputWrite); _hOutputWrite = nullptr; }

    emit disconnected();
}

void LocalShellTransport::write(const QByteArray& data)
{
    if (!_hInputWrite) return;
    DWORD written = 0;
    WriteFile(_hInputWrite, data.constData(),
              static_cast<DWORD>(data.size()), &written, nullptr);
}

void LocalShellTransport::resizeTerminal(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    _cols = cols;
    _rows = rows;

    if (_hPC) {
        COORD sz = { static_cast<SHORT>(cols), static_cast<SHORT>(rows) };
        pResizePseudoConsole(_hPC, sz);
    }
}

// ── private helpers ──────────────────────────────────────────────

bool LocalShellTransport::createPipes()
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

    // Input pipe (our write → ConPTY reads → app stdin)
    if (!CreatePipe(&_hInputPipe, &_hInputWrite, &sa, 0)) {
        _errorString = QStringLiteral("CreatePipe(input) failed: ") + lastWinError();
        qWarning() << "LocalShellTransport:" << _errorString;
        return false;
    }

    // Output pipe (app stdout → ConPTY writes → we read)
    if (!CreatePipe(&_hOutputRead, &_hOutputWrite, &sa, 0)) {
        _errorString = QStringLiteral("CreatePipe(output) failed: ") + lastWinError();
        qWarning() << "LocalShellTransport:" << _errorString;
        return false;
    }

    return true;
}

bool LocalShellTransport::createConPty(int cols, int rows)
{
    COORD sz = { static_cast<SHORT>(cols), static_cast<SHORT>(rows) };
    HRESULT hr = pCreatePseudoConsole(sz, _hInputPipe, _hOutputWrite, 0, &_hPC);
    if (FAILED(hr)) {
        _errorString = QStringLiteral("CreatePseudoConsole failed: ")
                       + winError(static_cast<DWORD>(hr));
        qWarning() << "LocalShellTransport:" << _errorString;
        return false;
    }
    return true;
}

bool LocalShellTransport::launchProcess(const QString& cmd)
{
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);

    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!siEx.lpAttributeList) {
        _errorString = QStringLiteral("HeapAlloc failed");
        return false;
    }

    if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize)) {
        _errorString = QStringLiteral("InitializeProcThreadAttributeList failed: ")
                       + lastWinError();
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        return false;
    }

    if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   _hPC, sizeof(HPCON), nullptr, nullptr)) {
        _errorString = QStringLiteral("UpdateProcThreadAttribute failed: ")
                       + lastWinError();
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        return false;
    }

    PROCESS_INFORMATION pi = {};
    std::wstring wcmd = cmd.toStdWString();
    std::wstring cmdBuf = wcmd;  // writable copy for CreateProcessW

    BOOL ok = CreateProcessW(
        nullptr, cmdBuf.data(),
        nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr,
        &siEx.StartupInfo, &pi);

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    // 释放已交给 ConPTY 的管道端 — 现在 ConPTY 拥有它们
    CloseHandle(_hInputPipe);   _hInputPipe   = nullptr;
    CloseHandle(_hOutputWrite); _hOutputWrite = nullptr;

    if (!ok) {
        _errorString = QStringLiteral("CreateProcess failed: ") + lastWinError();
        qWarning() << "LocalShellTransport:" << _errorString;
        return false;
    }

    _hProcess = pi.hProcess;
    _hThread  = pi.hThread;
    return true;
}

#endif // _WIN32
