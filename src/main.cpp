/**
 * @file main.cpp
 * @brief NovaTerm 应用程序入口文件
 *
 * 本文件实现了 NovaTerm 终端模拟器的程序入口点 main()，
 * 以及跨平台的崩溃转储（MiniDump/Core Dump）功能和 Windows
 * 平台特有的 DWM 窗口防闪烁处理。
 *
 * 主要功能：
 * - 平台相关环境变量初始化（字体引擎、RHI 后端等）
 * - 高 DPI 缩放属性配置
 * - QApplication 及 Application 单例的启动与关闭
 * - Windows：崩溃时生成 MiniDump (.dmp) 文件
 * - Windows：启动时通过 DWMWA_CLOAK 防止深色主题白底闪烁
 * - Linux：启用 Core Dump 以便崩溃后调试
 */

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

/**
 * @brief 动态调用 DwmSetWindowAttribute 设置窗口的 DWM 斗篷属性
 *
 * 该属性可让 DWM 停止或恢复合成指定窗口——窗口仍处于"可见"状态，
 * 但 DWM 不会将其绘制到屏幕上。用于在首帧渲染完成前隐藏窗口，
 * 避免深色主题启动时出现白底闪烁。Windows 8+ 可用。
 *
 * 通过 GetProcAddress 动态加载函数，避免硬链接 dwmapi.lib，
 * 保证在不支持该 API 的旧系统上程序仍可正常启动。
 *
 * @param hwnd 目标窗口的原生 HWND 句柄
 * @param cloak true 表示从 DWM 合成中隐藏（斗篷），false 表示恢复显示
 */
static void setDwmCloak(HWND hwnd, bool cloak)
{
    /// DwmSetWindowAttribute 函数指针类型别名
    using DwmSetWindowAttributeFunc = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    // 仅在首次调用时解析一次符号，后续调用复用函数指针
    static auto pDwmSetWindowAttribute = reinterpret_cast<DwmSetWindowAttributeFunc>(
        GetProcAddress(GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute"));
    if (pDwmSetWindowAttribute) {
        BOOL value = cloak ? TRUE : FALSE;
        pDwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
    }
}

/**
 * @brief 未处理异常过滤器：程序崩溃时自动生成 MiniDump (.dmp) 文件
 *
 * 功能等效于 Linux 的 core dump，生成的 .dmp 文件可在
 * Visual Studio 或 WinDbg 中加载以调试崩溃现场。
 * dump 文件生成在可执行文件所在目录，文件名包含时间戳，
 * 格式示例：NovaTerm_20260705_163025.dmp。
 *
 * @param pExceptionInfo 异常信息指针，包含异常记录和上下文寄存器
 * @return 始终返回 EXCEPTION_EXECUTE_HANDLER，让系统继续执行
 *         默认崩溃处理（弹出错误对话框等）
 */
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

        // 保持崩溃报告轻量。若在异常过滤器中同步写入完整地址空间，
        // 耗时过长会导致驱动崩溃看起来像是应用程序无响应。
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

/**
 * @brief 注册 MiniDump 崩溃处理回调
 *
 * 将 unhandledExceptionFilter 设置为进程级未处理异常过滤器，
 * 程序发生未捕获异常时自动生成 .dmp 转储文件。
 * 需在 main() 中窗口显示后调用，确保异常发生时必要的运行时已初始化。
 */
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


/**
 * @brief 启用 Linux Core Dump 生成功能
 *
 * 通过 setrlimit 将 RLIMIT_CORE 的软/硬限制均设为 RLIM_INFINITY，
 * 允许程序崩溃时生成无大小限制的 core 文件，便于后续使用
 * gdb/lldb 等调试器分析崩溃现场。若设置失败则通过
 * std::perror 输出错误信息（不终止程序）。
 *
 * @note 实际 core 文件的生成位置与命名还受 /proc/sys/kernel/core_pattern
 *       等系统内核参数影响。
 */
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


/**
 * @brief NovaTerm 应用程序入口点
 *
 * 程序启动的主流程：
 * 1. 设置平台相关环境变量（Windows 字体引擎、RHI 渲染后端）
 * 2. 配置高 DPI 缩放属性
 * 3. 创建 QApplication 实例并设置应用元信息
 * 4. 初始化 Application 单例（Ela 主题、翻译器、主窗口构建）
 * 5. 注册崩溃转储回调（MiniDump / Core Dump）
 * 6. 显示主窗口（Windows 下使用 DWM Cloak 防止深色主题白底闪烁）
 * 7. 进入 Qt 事件循环
 * 8. 事件循环退出后主动销毁主窗口，避免 atexit 阶段崩溃
 *
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return QApplication 事件循环的退出码，0 表示正常退出
 */
int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // 在部分 Windows 系统上，当 Qt 检查本地化字体元数据时，
    // DirectWrite 的字体族枚举会在 DWrite 内部崩溃。因此改用
    // Qt 支持的 FreeType 字体引擎，使终端字体匹配绕过该代码路径。
    // 若用户或测试已显式设置平台选择，则保留原值不覆盖。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");

    // 启动会话时 QRhiWidget 可能在主窗口已可见之后才创建，
    // Qt 6.8 需要提前准备顶层 backing store 以供 RHI 合成使用。
    // 保持 backing store 与终端渲染使用同一 API。D3D11 是 Qt 在
    // Windows 上成熟稳定的默认后端；此前会暴露驱动析构崩溃的
    // 会话对话框生命周期问题已另行修复。显式环境变量覆盖始终有效。
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
