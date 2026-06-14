#include "Application.h"
#include "MainWindow.h"
#include "ElaApplication.h"
#include "ElaTheme.h"
#include "core/LanguageManager.h"
#include "core/ConfigManager.h"
#include "pages/SettingsPage.h"

Application& Application::instance()
{
    // Meyers 单例 — 线程安全的延迟初始化，程序退出时销毁。
    static Application app;
    return app;
}

void Application::init()
{
    // 顺序很重要：
    //   1. eApp->init() 启动 ElaWidgetTools 运行时（主题、字体、特效）；
    //      必须在任何 Ela 控件构造之前调用。
    //   2. 从 windtermqt.json（位于可执行文件同目录）加载持久化配置。
    //   3. 应用已保存的语言和主题，若缺失则回退到内置默认值。
    //   4. 然后构建主窗口，使控件在首次绘制时即可看到最终状态。
    eApp->init();

    // 加载（或创建）持久化配置
    ConfigManager::instance().load();

    // ── 语言 ──────────────────────────────────────────────
    QString savedLang = ConfigManager::get<QString>("ui.language");
    LanguageManager::instance().install(savedLang);

    // ── 主题 ─────────────────────────────────────────────
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

    _mainWindow = std::make_unique<MainWindow>();
}
