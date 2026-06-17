#include "Application.h"
#include "MainWindow.h"
#include "ElaApplication.h"
#include "ElaTheme.h"
#include "core/LanguageManager.h"
#include "core/ConfigManager.h"
#include "pages/SettingsPage.h"

#include <QApplication>
#include <QPalette>

#ifdef Q_OS_WIN
#include <QCoreApplication>
#endif

Application& Application::instance()
{
    // Meyers 单例 — 线程安全的延迟初始化，程序退出时销毁。
    static Application app;
    return app;
}

void Application::init()
{
    // 顺序很重要：
    //   1. 先从配置文件读取主题设置，若为 "auto" 则检测系统主题；
    //      必须在 eApp->init() 之前设置，因为 ElaApplication 构造时会读取
    //      eTheme->getThemeMode() 来初始化内部状态。
    //      提前设置主题可避免跟随系统时，系统为深色主题而程序启动出现
    //      短暂的白色主题闪烁。
    //   2. eApp->init() 启动 ElaWidgetTools 运行时（主题、字体、特效）；
    //      必须在任何 Ela 控件构造之前调用。
    //   3. 加载完整持久化配置并应用语言。
    //   4. 然后构建主窗口，使控件在首次绘制时即可看到最终状态。

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

    // ElaWindow 的 stylesheet 将背景设为 transparent，因此原生窗口的默认
    // 背景色会透出。在深色主题下将 QPalette::Window 设为深色，避免 show()
    // 时原生窗口短暂显示白色背景。
    if (eTheme->getThemeMode() == ElaThemeType::Dark) {
        auto* app = static_cast<QApplication*>(QCoreApplication::instance());
        QPalette p;  // 干净基准（平台无关浅色默认值）；不可从 app->palette() 复制，
                     // 否则 Mid / Dark / Shadow 等 QScrollBar 角色残留浅色主题值。
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
    }

    // 第三步：初始化 ElaWidgetTools 运行时
    eApp->init();

    // ── 语言 ──────────────────────────────────────────────
    QString savedLang = ConfigManager::get<QString>("ui.language");
    LanguageManager::instance().install(savedLang);

    _mainWindow = std::make_unique<MainWindow>();

    // ElaWindow 构造函数中 setObjectName("ElaWindow") 并设
    // stylesheet #ElaWindow{background:transparent}。
    // 窗口拖动时 Qt 不填充 backing store，脏帧透出造成闪烁。
    // 两重修复：
    //   1. 改 objectName 使原 #ElaWindow 选择器失效
    //   2. 追加 #MainWindow 选择器用主题色填充背景
    _mainWindow->setObjectName("NovaTermMainWindow");
    _mainWindow->setAutoFillBackground(true);
    {
        const QColor bg = eTheme->getThemeMode() == ElaThemeType::Dark
                              ? QColor(0x20, 0x20, 0x20)
                              : QColor(0xEC, 0xEC, 0xEC);
        QPalette p = _mainWindow->palette();
        p.setColor(QPalette::Window, bg);
        _mainWindow->setPalette(p);
        _mainWindow->setStyleSheet(
            _mainWindow->styleSheet()
            + QStringLiteral("\n#NovaTermMainWindow { background-color: %1; }")
                  .arg(bg.name()));
    }

#ifdef Q_OS_WIN
    // 全局安装系统主题变更监听，不依赖 SettingsPage 是否打开
    static ThemeChangeWatcher themeWatcher;
    QCoreApplication::instance()->installNativeEventFilter(&themeWatcher);
#endif
}
