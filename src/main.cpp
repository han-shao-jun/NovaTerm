#include "ui/app/Application.h"
#include "ui/app/MainWindow.h"
#include <QApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#include <time.h>

#pragma comment(lib, "dbghelp.lib")

// DWMWA_CLOAK (13) 让 DWM 停止合成该窗口 —— 窗口仍然"可见"，
// 但 DWM 不会将其绘制到屏幕上。Windows 8+ 可用。
#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

// 动态加载 DwmSetWindowAttribute，避免硬链接 dwmapi.lib。
static void setDwmCloak(HWND hwnd, bool cloak)
{

    using DwmSetWindowAttributeFunc = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static auto pDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFunc>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
    if (pDwmSetWindowAttribute) {
        BOOL value = cloak ? TRUE : FALSE;
        pDwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
    }
}

// ── 未处理异常过滤器：崩溃时自动生成 MiniDump (.dmp 文件) ──────
// 效果等同于 Linux 的 core dump，可用 Visual Studio 或 WinDbg 打开调试。
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pExceptionInfo)
{
    // 在 exe 同目录生成带时间戳的 dump 文件名，如 NovaTerm_20260705_163025.dmp
    wchar_t fileName[MAX_PATH];
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(dir, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_s(&tmNow, &now);
    swprintf_s(fileName, MAX_PATH, L"%sNovaTerm_%04d%02d%02d_%02d%02d%02d.dmp",
               dir,
               tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
               tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

    HANDLE hFile = CreateFileW(fileName, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = pExceptionInfo;
        mei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          MiniDumpWithFullMemory, &mei, nullptr, nullptr);

        CloseHandle(hFile);
    }

    return EXCEPTION_EXECUTE_HANDLER; // 让系统继续执行默认处理（弹出错误对话框等）
}

static void enableMiniDump()
{
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
}

#else

#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>

static void generateCoreDump(int signo)
{
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);

    char fileName[512];
    const char* dir = QCoreApplication::applicationDirPath().toLocal8Bit().constData();
    snprintf(fileName, sizeof(fileName), "%s/NovaTerm_%04d%02d%02d_%02d%02d%02d_%d.core",
             dir,
             tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
             tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
             getpid());

    fprintf(stderr,
            "\n*** NovaTerm CRASH (signal %d) ***\n"
            "Time: %04d-%02d-%02d %02d:%02d:%02d  PID: %d\n"
            "Core dump: %s\n",
            signo,
            tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
            tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec, getpid(),
            fileName);
    fflush(stderr);

    // Fork → 孙进程：等内核写出 core 文件后重命名为时间戳文件名。
    // 不能直接用 gdb -p 生成 core：gdb 的 ptrace 会挂起父进程导致程序"卡死"。
    // 这里让父进程立即 raise 死亡，内核异步写入 core，孙进程负责重命名。
    pid_t child = fork();
    if (child == 0) {
        // 脱离父进程，避免成为僵尸
        setsid();
        pid_t gc = fork();
        if (gc == 0) {
            // 孙进程：轮询等待内核写出 core 文件，最多等 5 秒
            char corePath[512];
            snprintf(corePath, sizeof(corePath), "%s/core", dir);
            for (int i = 0; i < 50; ++i) {
                usleep(100000);  // 100ms
                struct stat st;
                if (stat(corePath, &st) == 0 && st.st_size > 0) {
                    rename(corePath, fileName);
                    break;
                }
            }
            _exit(0);
        }
        _exit(0);  // 子进程立即退出，孙进程变为孤儿
    }

    // 父进程：恢复默认信号处理 → 立即死亡，内核生成 core 文件
    signal(signo, SIG_DFL);
    raise(signo);
}

static void enableCoreDump()
{
    struct rlimit rl;
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &rl);
    
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = generateCoreDump;
    
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

#endif


int main(int argc, char *argv[])
{
    // QRhiWidget needs the top-level QWidget backing store to use QRhi
    // composition. Configure it before QApplication initializes the platform.
    if (qEnvironmentVariableIsEmpty("QT_WIDGETS_RHI"))
        qputenv("QT_WIDGETS_RHI", "1");

    // 高 DPI 处理。Qt 6 默认启用高 DPI 缩放，以下属性仅在 Qt 5 上需要。
    // PassThrough 保留分数缩放比例（如 150%）而非取整，确保渲染清晰。
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
#endif
    QApplication a(argc, argv);
    // 应用程序标识，用于 QSettings 存储路径和任务栏入口名称。
    a.setApplicationName("NovaTerm");
    a.setApplicationVersion("0.1.0");
    a.setOrganizationName("NovaTerm");

    // init() 必须在 show() 之前调用：启动 ElaApplication，安装翻译器，
    // 并构建主窗口（参见 Application::init）。
    Application::instance().init();
    auto& w = Application::instance().mainWindow();

#ifdef Q_OS_WIN
    enableMiniDump();  // 注册崩溃处理：程序崩溃时自动生成 .dmp 文件

    // ── 防止深色主题启动时的白底闪烁 ───────────────────
    // 策略：在窗口首次变为可见之前，用 DWMWA_CLOAK 将其从 DWM
    // 合成中隐藏，渲染完成后深色内容，再解除 Cloak 让 DWM 显示。
    // 这样用户永远看不到未渲染的默认白色窗口表面。
    //
    // winId() 强制创建原生 HWND（仍处于隐藏状态），以便在 show()
    // 之前设置 Cloak 属性。
    HWND hwnd = reinterpret_cast<HWND>(w.winId());
    setDwmCloak(hwnd, true);   // 1. 从 DWM 合成中隐藏
    w.show();                  // 2. 窗口"显示"（DWM 不合成，用户看不到）

    // 3. 同步完成擦除背景 + 首次绘制，将深色内容写入 DWM 表面
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);

    QApplication::processEvents();
    setDwmCloak(hwnd, false);  // 4. 恢复 DWM 合成 → 首帧即深色

#else
    w.show();

    enableCoreDump();
#endif

    const int rc = a.exec();

    // 在 QApplication 仍存活时销毁 MainWindow。Application 是函数内 static
    // 单例，若依赖其默认析构，MainWindow 会在 atexit 阶段（QApplication 已
    // 析构之后）才销毁，引发退出时崩溃（详见 Application::shutdown()）。
    Application::instance().shutdown();

    return rc;
}
