#include "app/Application.h"
#include "app/MainWindow.h"
#include <QApplication>

#ifdef Q_OS_WIN
#include <windows.h>

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

#else

#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

static void enableCoreDump()
{
    struct rlimit rl;
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;

    setrlimit(RLIMIT_CORE, &rl);
}

#endif


int main(int argc, char *argv[])
{
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
    // ── 防止深色主题启动时的白底闪烁 ───────────────────
    // 策略：在窗口首次变为可见之前，用 DWMWA_CLOAK 将其从 DWM
    // 合成中隐藏，渲染完成后深色内容，再解除 Cloak 让 DWM 显示。
    // 这样用户永远看不到未渲染的默认白色窗口表面。
    //
    // winId() 强制创建原生 HWND（仍处于隐藏状态），以便在 show()
    // 之前设置 Cloak 属性。
    HWND hwnd = reinterpret_cast<HWND>(w.winId());
    setDwmCloak(hwnd, true);   // 1. 从 DWM 合成中隐藏
    w.show();                   // 2. 窗口"显示"（DWM 不合成，用户看不到）

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
