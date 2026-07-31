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

        // Keep crash reporting lightweight. Writing the complete address space
        // synchronously from this exception filter can take a long time and
        // makes a driver crash look like an application hang.
        constexpr auto dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal
            | MiniDumpWithThreadInfo
            | MiniDumpWithUnloadedModules
            | MiniDumpScanMemory
            | MiniDumpWithIndirectlyReferencedMemory);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          dumpType, &mei, nullptr, nullptr);

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


static void enableCoreDump()
{
    struct rlimit limit {};
    limit.rlim_cur = RLIM_INFINITY;
    limit.rlim_max = RLIM_INFINITY;

    if (setrlimit(RLIMIT_CORE, &limit) != 0) {
        std::perror("setrlimit");
    }
}

#endif


int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // DirectWrite font-family enumeration crashes inside DWrite on some
    // Windows installations when Qt inspects localized font metadata. Use
    // Qt's supported FreeType font engine so terminal font matching bypasses
    // that path. Preserve an explicit platform selection from users or tests.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");

    // QRhiWidget can be created after the already-visible main window when a
    // session starts. Qt 6.8 needs the top-level backing store prepared for
    // RHI composition in that case. Keep the backing store and terminal on
    // the same API. D3D11 is Qt's mature Windows default; the session dialog
    // lifetime bug that previously exposed a driver teardown crash is fixed
    // separately. Explicit environment overrides remain supported.
    if (qEnvironmentVariableIsEmpty("QT_WIDGETS_RHI"))
        qputenv("QT_WIDGETS_RHI", "1");
    if (qEnvironmentVariableIsEmpty("QT_WIDGETS_RHI_BACKEND"))
        qputenv("QT_WIDGETS_RHI_BACKEND", "d3d11");
    if (qEnvironmentVariableIsEmpty("NOVATERM_RHI_API"))
        qputenv("NOVATERM_RHI_API", "d3d11");
#endif

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
