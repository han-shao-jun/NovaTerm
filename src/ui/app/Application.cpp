/**
 * @file   Application.cpp
 * @brief  应用程序单例实现：启动流程与翻译加载。
 *
 * init() 依次确定主题、按主题设置应用调色板、初始化 ElaApplication、加载
 * LanguageManager 翻译、构建并显示 MainWindow。shutdown() 在 a.exec() 返回
 * 后调用，提前释放窗口避免退出崩溃。
 *
 * 原生 Qt 控件的深色外观依赖两件事：这里设置的 QApplication 调色板，以及
 * Windows 10 上由 main() 改用的 windows11 style（详见 main.cpp 的说明）。
 */
#include "Application.h"
#include "MainWindow.h"
#include "mcp/McpService.h"
#include "ElaApplication.h"
#include "ElaTheme.h"
#include "service/LanguageManager.h"
#include "service/ConfigManager.h"
#include "ui/pages/SettingsPage.h"

#include <QApplication>
#include <QPalette>

#ifdef Q_OS_WIN
#include <QCoreApplication>
#endif

Application::~Application() = default;

Application& Application::instance()
{
    // Meyers 单例 — 线程安全的延迟初始化，程序退出时销毁。
    static Application app;
    return app;
}

void Application::shutdown()
{
    if (_mcpService) _mcpService->stop();
    _mainWindow.reset();
    _mcpService.reset();
}

void Application::init()
{
    // 顺序很重要：
    //   1. 加载持久化配置。
    //   2. 从配置读取主题设置，若为 "auto" 则检测系统主题；
    //      必须在 eApp->init() 之前设置，因为 ElaApplication 构造时会读取
    //      eTheme->getThemeMode() 来初始化内部状态。
    //      提前设置主题可避免跟随系统时，系统为深色主题而程序启动出现
    //      短暂的白色主题闪烁。
    //   3. 按主题设置 QApplication 调色板，原生 Qt 控件靠它取色。
    //   4. eApp->init() 启动 ElaWidgetTools 运行时（主题、字体、特效）；
    //      必须在任何 Ela 控件构造之前调用。
    //   5. 应用语言，然后构建主窗口，使控件在首次绘制时即可看到最终状态。
    //
    // Windows 10 的 style 替换比这里更早，在 main() 里 QApplication 构造之前
    // 通过 QT_STYLE_OVERRIDE 完成（见 main.cpp）。

    // 第一步：加载（或创建）持久化配置，以便尽早确定主题
    ConfigManager::instance().load();

    // 第二步：在 eApp->init() 之前设置主题，防止默认亮色主题闪烁
    QString savedTheme = ConfigManager::get<QString>("ui.theme");
    if (savedTheme == "auto") {
        eTheme->setThemeMode(SettingsPage::isSystemDarkTheme()
                                 ? ElaThemeType::Dark
                                 : ElaThemeType::Light);
    } else if (savedTheme == "dark") {
        eTheme->setThemeMode(ElaThemeType::Dark);
    } else {
        eTheme->setThemeMode(ElaThemeType::Light);
    }

    // 第三步：按主题设置应用调色板。
    // ElaWindow 的 stylesheet 将背景设为 transparent，因此原生窗口的默认
    // 背景色会透出。在深色主题下将 QPalette::Window 设为深色，避免 show()
    // 时原生窗口短暂显示白色背景。
    //
    // 两个分支都必须显式设置：windows11 style 的 standardPalette() 跟随
    // **系统**配色而非本程序的主题设置（实测系统深色时给出 Window
    // #1e1e1e）。若亮色分支不设，"程序亮色 + 系统深色" 组合下原生 Qt 控件
    // 会反过来变深，与 Ela 控件的亮色不一致。
    // 这里的取色与 MainWindow 的 themeModeChanged 处理器保持一致，
    // 后者负责运行期切换主题时的同步 —— **两处的两个分支都必须设置同一套角色**，
    // 理由见 MainWindow 那段注释（`QPalette p;` 会拷贝当前 app palette，漏设的
    // 角色会残留对端主题的值，浅色下表现为白底白字）。
    {
        auto* app = static_cast<QApplication*>(QCoreApplication::instance());
        if (eTheme->getThemeMode() == ElaThemeType::Dark) {
            QPalette p;
            // ElaTheme 深色 WindowBase: #202020, BasicBase: #343434
            p.setColor(QPalette::Window,          QColor(0x20, 0x20, 0x20));
            p.setColor(QPalette::Base,            QColor(0x34, 0x34, 0x34));
            p.setColor(QPalette::AlternateBase,   QColor(0x2A, 0x2A, 0x2A));
            p.setColor(QPalette::WindowText,      QColor(0xF0, 0xF0, 0xF0));
            p.setColor(QPalette::Text,            QColor(0xF0, 0xF0, 0xF0));
            p.setColor(QPalette::Button,          QColor(0x34, 0x34, 0x34));
            p.setColor(QPalette::ButtonText,      QColor(0xF0, 0xF0, 0xF0));
            // 派生色 — QScrollBar 等原生控件用这些角色绘制
            p.setColor(QPalette::Mid,             QColor(0x40, 0x40, 0x40));
            p.setColor(QPalette::Dark,            QColor(0x18, 0x18, 0x18));
            p.setColor(QPalette::Shadow,          QColor(0x10, 0x10, 0x10));
            p.setColor(QPalette::Light,           QColor(0x48, 0x48, 0x48));
            p.setColor(QPalette::Midlight,        QColor(0x3C, 0x3C, 0x3C));
            p.setColor(QPalette::Highlight,       QColor(0x00, 0x78, 0xD4));
            p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
            p.setColor(QPalette::BrightText,      QColor(0xFF, 0x44, 0x44));
            p.setColor(QPalette::Link,            QColor(0x4D, 0xA6, 0xFF));
            app->setPalette(p);
        } else {
            QPalette p;
            // ElaTheme 浅色 WindowBase: #ECECEC, BasicBase: #FDFDFD,
            // BasicText: 黑, BasicHover: #F3F3F3, BasicBorderDeep: #9A9A9A
            p.setColor(QPalette::Window,          QColor(0xEC, 0xEC, 0xEC));
            p.setColor(QPalette::Base,            QColor(0xFF, 0xFF, 0xFF));
            p.setColor(QPalette::AlternateBase,   QColor(0xF7, 0xF7, 0xF7));
            p.setColor(QPalette::WindowText,      QColor(0x00, 0x00, 0x00));
            p.setColor(QPalette::Text,            QColor(0x00, 0x00, 0x00));
            p.setColor(QPalette::Button,          QColor(0xFD, 0xFD, 0xFD));
            p.setColor(QPalette::ButtonText,      QColor(0x00, 0x00, 0x00));
            // 派生色 — 与深色分支一一对应，缺一个就会残留对端主题的值
            p.setColor(QPalette::Mid,             QColor(0xC8, 0xC8, 0xC8));
            p.setColor(QPalette::Dark,            QColor(0x9A, 0x9A, 0x9A));
            p.setColor(QPalette::Shadow,          QColor(0x76, 0x76, 0x76));
            p.setColor(QPalette::Light,           QColor(0xFF, 0xFF, 0xFF));
            p.setColor(QPalette::Midlight,        QColor(0xF3, 0xF3, 0xF3));
            p.setColor(QPalette::Highlight,       QColor(0x00, 0x78, 0xD4));
            p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
            p.setColor(QPalette::BrightText,      QColor(0xC0, 0x00, 0x00));
            p.setColor(QPalette::Link,            QColor(0x00, 0x67, 0xC0));
            app->setPalette(p);
        }
    }

    // 第四步：初始化 ElaWidgetTools 运行时
    eApp->init();

    // ── 语言 ──────────────────────────────────────────────
    QString savedLang = ConfigManager::get<QString>("ui.language");
    LanguageManager::instance().install(savedLang);

    _mcpService = std::make_unique<NovaTerm::Mcp::Service>();
    _mainWindow = std::make_unique<MainWindow>();

    // ElaWindow 构造函数中 setObjectName("ElaWindow") 并设
    // 样式表 #ElaWindow{background:transparent}。
    // 窗口拖动时 Qt 不填充 backing store，脏帧透出造成闪烁。
    // 两重修复：
    //   1. 改 objectName 使原 #ElaWindow 选择器失效
    //   2. 追加 #NovaTermMainWindow 选择器用主题色填充背景，并随主题切换重写
    _mainWindow->setObjectName("NovaTermMainWindow");
    _mainWindow->setAutoFillBackground(true);
    {
        // ElaWindow 的原始样式表留作基准：下面按主题重写背景规则时必须以它为底
        // 重建，反复 append 会不断堆积同名规则。
        const QString baseStyleSheet = _mainWindow->styleSheet();
        auto* const window = _mainWindow.get();
        const auto applyWindowBackground =
            [window, baseStyleSheet](ElaThemeType::ThemeMode mode) {
            const QColor bg = mode == ElaThemeType::Dark
                                  ? QColor(0x20, 0x20, 0x20)
                                  : QColor(0xEC, 0xEC, 0xEC);
            QPalette palette = window->palette();
            palette.setColor(QPalette::Window, bg);
            window->setPalette(palette);
            window->setStyleSheet(
                baseStyleSheet
                + QStringLiteral("\n#NovaTermMainWindow { background-color: %1; }")
                      .arg(bg.name()));
        };
        applyWindowBackground(eTheme->getThemeMode());

        // 这条连接不能省：**QSS 的 background-color 优先级高于 QPalette**，所以
        // 只靠 MainWindow 的 themeModeChanged 处理器同步 palette 改不动窗口底色。
        // 漏掉它的后果是切换主题后底色停留在启动时的值 —— 深色启动再切浅色时，
        // 各控件都转成浅色而窗口底色仍是 #202020，「系统资源」这类大面积透出窗口
        // 背景的自绘面板会呈现深底 + 浅色控件的割裂外观。
        QObject::connect(eTheme, &ElaTheme::themeModeChanged, window,
                         applyWindowBackground);
    }

#ifdef Q_OS_WIN
    // 全局安装系统主题变更监听，不依赖 SettingsPage 是否打开
    static ThemeChangeWatcher themeWatcher;
    QCoreApplication::instance()->installNativeEventFilter(&themeWatcher);
#endif
}
