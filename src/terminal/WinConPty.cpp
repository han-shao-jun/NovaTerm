#ifdef _WIN32

#include "WinConPty.h"
#include <QCoreApplication>
#include <QDebug>
#include <sstream>
#include <atomic>

// ── helpers ────────────────────────────────────────────────────
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

// Dynamically resolve ConPTY functions (not in all SDKs;
// the app may also run on pre-1809 Windows)
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

// ────────────────────────────────────────────────────────────────
//  WinConPty
// ────────────────────────────────────────────────────────────────

WinConPty::WinConPty(QObject *parent)
    : QObject(parent)
{
}

WinConPty::~WinConPty()
{
    stop();
}

bool WinConPty::start(const QString &shell, const QStringList &args)
{
    if (!resolveConPty()) {
        qWarning() << "WinConPty: ConPTY not available (need Windows 10 1809+)";
        return false;
    }

    if (_running) stop();

    if (!createPipes()) return false;
    if (!createConPty(80, 24)) return false;

    // build command line
    std::wstringstream cmdLine;
    cmdLine << L"\"" << shell.toStdWString() << L"\"";
    for (const QString &a : args)
        cmdLine << L" " << a.toStdWString();

    if (!launchProcess(QString::fromStdWString(cmdLine.str()))) return false;

    // Start reader thread
    _running = true;
    _readerThread = QThread::create([this]() {
        char buf[4096];
        DWORD n;
        while (_running.load() &&
               ReadFile(_hOutputRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            QByteArray data(buf, static_cast<int>(n));
            QMetaObject::invokeMethod(this, [this, d = std::move(data)]() {
                if (_running.load())
                    emit receivedData(d.constData(), d.size());
            }, Qt::QueuedConnection);
        }
    });
    _readerThread->start();
    return true;
}

void WinConPty::stop()
{
    _running = false;

    // Remove any invokeMethod events still queued for this object
    QCoreApplication::removePostedEvents(this);

    // Close ConPTY first — forces ReadFile in reader thread to return
    if (_hPC) {
        pClosePseudoConsole(_hPC);
        _hPC = nullptr;
    }

    DWORD exitCode = 0;
    if (_hProcess) {
        TerminateProcess(_hProcess, 0);
        WaitForSingleObject(_hProcess, 2000);
        GetExitCodeProcess(_hProcess, &exitCode);
        CloseHandle(_hProcess);
        _hProcess = nullptr;
    }
    if (_hThread) {
        CloseHandle(_hThread);
        _hThread = nullptr;
    }

    if (_readerThread) {
        _readerThread->wait(3000);
        delete _readerThread;
        _readerThread = nullptr;
    }

    if (_hInputWrite)  { CloseHandle(_hInputWrite);  _hInputWrite  = nullptr; }
    if (_hInputPipe)   { CloseHandle(_hInputPipe);   _hInputPipe   = nullptr; }
    if (_hOutputRead)  { CloseHandle(_hOutputRead);  _hOutputRead  = nullptr; }
    if (_hOutputWrite) { CloseHandle(_hOutputWrite); _hOutputWrite = nullptr; }

    emit finished(static_cast<int>(exitCode));
}

bool WinConPty::isRunning() const
{
    return _running;
}

void WinConPty::writeData(const char *data, int len)
{
    if (!_hInputWrite) return;
    DWORD written = 0;
    WriteFile(_hInputWrite, data, static_cast<DWORD>(len), &written, nullptr);
}

void WinConPty::resize(int cols, int rows)
{
    if (_hPC) {
        COORD sz = { static_cast<SHORT>(cols), static_cast<SHORT>(rows) };
        pResizePseudoConsole(_hPC, sz);
    }
}

// ── private helpers ────────────────────────────────────────────

bool WinConPty::createPipes()
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

    // Input pipe (our write → ConPTY reads → app stdin)
    if (!CreatePipe(&_hInputPipe, &_hInputWrite, &sa, 0)) {
        qWarning() << "WinConPty: CreatePipe(input) failed:" << lastWinError();
        return false;
    }

    // Output pipe (app stdout → ConPTY writes → we read)
    if (!CreatePipe(&_hOutputRead, &_hOutputWrite, &sa, 0)) {
        qWarning() << "WinConPty: CreatePipe(output) failed:" << lastWinError();
        return false;
    }

    return true;
}

bool WinConPty::createConPty(int cols, int rows)
{
    COORD sz = { static_cast<SHORT>(cols), static_cast<SHORT>(rows) };
    HRESULT hr = pCreatePseudoConsole(sz, _hInputPipe, _hOutputWrite, 0, &_hPC);
    if (FAILED(hr)) {
        qWarning() << "WinConPty: CreatePseudoConsole failed:" << winError(static_cast<DWORD>(hr));
        return false;
    }
    return true;
}

bool WinConPty::launchProcess(const QString &cmd)
{
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);

    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!siEx.lpAttributeList) return false;

    if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize)) {
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        return false;
    }

    if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   _hPC, sizeof(HPCON), nullptr, nullptr)) {
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

    // Release pipe ends handed to ConPTY — it now owns them
    CloseHandle(_hInputPipe);   _hInputPipe   = nullptr;
    CloseHandle(_hOutputWrite); _hOutputWrite = nullptr;

    if (!ok) {
        qWarning() << "WinConPty: CreateProcess failed:" << lastWinError();
        return false;
    }

    _hProcess = pi.hProcess;
    _hThread  = pi.hThread;
    return true;
}

#endif // _WIN32
