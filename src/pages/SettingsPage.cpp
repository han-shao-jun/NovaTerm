#include "SettingsPage.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QProcess>
#include <QSettings>
#include <QVBoxLayout>

#include "ElaApplication.h"
#include "ElaComboBox.h"
#include "ElaRadioButton.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaToggleSwitch.h"
#include "ElaWindow.h"
#include "core/LanguageManager.h"
#include "core/ConfigManager.h"

// ── 系统主题检测 ──────────────────────────────────────

bool SettingsPage::isSystemDarkTheme()
{
#ifdef Q_OS_WIN
    QSettings reg(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        QSettings::NativeFormat);
    // AppsUseLightTheme: 1 = 浅色, 0 = 深色
    return reg.value("AppsUseLightTheme", 1).toInt() == 0;
#elif defined(Q_OS_MACOS)
    // macOS 深色模式 — CFPreferences
    //   "AppleInterfaceStyle" == "Dark" → 深色
    QSettings macSettings(
        "Apple Global Domain",
        QSettings::NativeFormat);
    return macSettings.value("AppleInterfaceStyle").toString() == "Dark";
#else
    // Linux — 尝试 gsettings 或 xdg-desktop-portal
    // 最可靠的方式：检查环境变量或 gsettings
    QProcess proc;
    proc.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
    proc.waitForFinished(2000);
    QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    if (output.contains("dark") || output.contains("prefer-dark"))
        return true;
    // 回退：检查 GTK_THEME 或 QT_QUICK_CONTROLS_STYLE 环境变量
    return false;
#endif
}

void SettingsPage::applyAutoTheme()
{
    eTheme->setThemeMode(isSystemDarkTheme() ? ElaThemeType::Dark : ElaThemeType::Light);
}

void SettingsPage::onThemeComboChanged(int index)
{
    // 将主题选择持久化到配置
    if (index == _themeAutoComboIndex) {
        ConfigManager::set("ui.theme", "auto");
    } else {
        ConfigManager::set("ui.theme", index == 0 ? "light" : "dark");
    }

    if (index == _themeAutoComboIndex) {
        _themeAutoMode = true;
        applyAutoTheme();
#ifdef Q_OS_WIN
        ThemeChangeWatcher::instance().setAutoPage(this);
#endif
    } else {
        _themeAutoMode = false;
#ifdef Q_OS_WIN
        ThemeChangeWatcher::instance().setAutoPage(nullptr);
#endif
        eTheme->setThemeMode(index == 0 ? ElaThemeType::Light : ElaThemeType::Dark);
    }
}

void SettingsPage::onElaThemeChanged(ElaThemeType::ThemeMode mode)
{
    _themeComboBox->blockSignals(true);
    if (_themeAutoMode) {
        _themeComboBox->setCurrentIndex(_themeAutoComboIndex);
    } else {
        _themeComboBox->setCurrentIndex(mode == ElaThemeType::Light ? 0 : 1);
    }
    _themeComboBox->blockSignals(false);
}

// ── Windows 主题变更监听器 ──────────────────────────────

#ifdef Q_OS_WIN
#include <windows.h>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>

ThemeChangeWatcher& ThemeChangeWatcher::instance()
{
    static ThemeChangeWatcher watcher;
    return watcher;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool ThemeChangeWatcher::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
#else
bool ThemeChangeWatcher::nativeEventFilter(const QByteArray& eventType, void* message, long* result)
#endif
{
    Q_UNUSED(result)
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_SETTINGCHANGE && _page) {
            _page->applyAutoTheme();
        }
    }
    return false;
}
#endif

// ── 构造函数 ─────────────────────────────────────────────

SettingsPage::SettingsPage(QWidget* parent)
    : ElaScrollPage(parent)
{
    setWindowTitle(tr("Settings"));

    ElaWindow* window = dynamic_cast<ElaWindow*>(parent);

    // ── 语言 ───────────────────────────────────────────
    _langSectionText = new ElaText(tr("Language"), this);
    _langSectionText->setWordWrap(false);
    _langSectionText->setTextPixelSize(18);

    _languageComboBox = new ElaComboBox(this);
    _languageComboBox->addItem(tr("English"), "en");
    _languageComboBox->addItem(tr("简体中文"), "zh_CN");
    // 选择当前语言
    QString curLang = LanguageManager::instance().currentLocale();
    for (int i = 0; i < _languageComboBox->count(); ++i) {
        if (_languageComboBox->itemData(i).toString() == curLang) {
            _languageComboBox->setCurrentIndex(i);
            break;
        }
    }
    ElaScrollPageArea* langArea = new ElaScrollPageArea(this);
    QHBoxLayout* langLayout = new QHBoxLayout(langArea);
    _langLabel = new ElaText(tr("Language"), this);
    _langLabel->setWordWrap(false);
    _langLabel->setTextPixelSize(15);
    langLayout->addWidget(_langLabel);
    langLayout->addStretch();
    langLayout->addWidget(_languageComboBox);
    connect(_languageComboBox, QOverload<int>::of(&ElaComboBox::currentIndexChanged),
            this, [=](int index) {
        QString locale = _languageComboBox->itemData(index).toString();
        LanguageManager::instance().switchLanguage(locale);
        ConfigManager::set("ui.language", locale);
    });

    // ── 动态语言切换 ─────────────────────────
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    // ── 主题 ──────────────────────────────────────────────
    _themeSectionText = new ElaText(tr("Theme"), this);
    _themeSectionText->setWordWrap(false);
    _themeSectionText->setTextPixelSize(18);

    _themeComboBox = new ElaComboBox(this);
    _themeComboBox->addItem(tr("Light"));
    _themeComboBox->addItem(tr("Dark"));
    _themeComboBox->addItem(tr("Auto (System)"));
    _themeAutoComboIndex = 2;

    // 启动时应用系统主题
    _themeAutoMode = true;
    applyAutoTheme();
    _themeComboBox->setCurrentIndex(_themeAutoComboIndex);

#ifdef Q_OS_WIN
    QCoreApplication::instance()->installNativeEventFilter(&ThemeChangeWatcher::instance());
    ThemeChangeWatcher::instance().setAutoPage(this);
#endif

    ElaScrollPageArea* themeSwitchArea = new ElaScrollPageArea(this);
    QHBoxLayout* themeSwitchLayout = new QHBoxLayout(themeSwitchArea);
    _themeSwitchText = new ElaText(tr("Theme Mode"), this);
    _themeSwitchText->setWordWrap(false);
    _themeSwitchText->setTextPixelSize(15);
    themeSwitchLayout->addWidget(_themeSwitchText);
    themeSwitchLayout->addStretch();
    themeSwitchLayout->addWidget(_themeComboBox);

    connect(_themeComboBox, QOverload<int>::of(&ElaComboBox::currentIndexChanged),
            this, &SettingsPage::onThemeComboChanged);
    connect(eTheme, &ElaTheme::themeModeChanged, this, &SettingsPage::onElaThemeChanged);

    // ── 窗口绘制模式 ────────────────────────────────
    _windowPaintText = new ElaText(tr("Window Paint Mode"), this);
    _windowPaintText->setWordWrap(false);
    _windowPaintText->setTextPixelSize(15);

    _windowNormalButton = new ElaRadioButton(tr("Normal"), this);
    _windowNormalButton->setChecked(true);
    _windowPixmapButton = new ElaRadioButton(tr("Pixmap"), this);
    _windowMovieButton  = new ElaRadioButton(tr("Movie"), this);

    QButtonGroup* windowPaintGroup = new QButtonGroup(this);
    windowPaintGroup->addButton(_windowNormalButton, 0);
    windowPaintGroup->addButton(_windowPixmapButton, 1);
    windowPaintGroup->addButton(_windowMovieButton, 2);
    connect(windowPaintGroup, QOverload<QAbstractButton*, bool>::of(&QButtonGroup::buttonToggled),
            this, [=](QAbstractButton* button, bool isToggled) {
        if (isToggled && window)
            window->setWindowPaintMode(
                static_cast<ElaWindowType::PaintMode>(windowPaintGroup->id(button)));
    });
    ElaScrollPageArea* windowPaintArea = new ElaScrollPageArea(this);
    QHBoxLayout* windowPaintLayout = new QHBoxLayout(windowPaintArea);
    windowPaintLayout->addWidget(_windowPaintText);
    windowPaintLayout->addStretch();
    windowPaintLayout->addWidget(_windowNormalButton);
    windowPaintLayout->addWidget(_windowPixmapButton);
    windowPaintLayout->addWidget(_windowMovieButton);

    // ── 应用程序设置 ──────────────────────────────
    _appSectionText = new ElaText(tr("Application"), this);
    _appSectionText->setWordWrap(false);
    _appSectionText->setTextPixelSize(18);

    // 窗口特效
    _micaText = new ElaText(tr("Window Effect"), this);
    _micaText->setWordWrap(false);
    _micaText->setTextPixelSize(15);
    _normalButton = new ElaRadioButton(tr("Normal"), this);
    _elaMicaButton = new ElaRadioButton(tr("ElaMica"), this);
#ifdef Q_OS_WIN
    _micaButton    = new ElaRadioButton(tr("Mica"), this);
    _micaAltButton = new ElaRadioButton(tr("Mica-Alt"), this);
    _acrylicButton = new ElaRadioButton(tr("Acrylic"), this);
    _dwmBlurButton = new ElaRadioButton(tr("Dwm-Blur"), this);
#endif
    _normalButton->setChecked(true);
    QButtonGroup* displayGroup = new QButtonGroup(this);
    displayGroup->addButton(_normalButton, 0);
    displayGroup->addButton(_elaMicaButton, 1);
#ifdef Q_OS_WIN
    displayGroup->addButton(_micaButton, 2);
    displayGroup->addButton(_micaAltButton, 3);
    displayGroup->addButton(_acrylicButton, 4);
    displayGroup->addButton(_dwmBlurButton, 5);
#endif
    connect(displayGroup, QOverload<QAbstractButton*, bool>::of(&QButtonGroup::buttonToggled),
            this, [=](QAbstractButton* button, bool isToggled) {
        if (isToggled)
            eApp->setWindowDisplayMode(
                static_cast<ElaApplicationType::WindowDisplayMode>(displayGroup->id(button)));
    });
    ElaScrollPageArea* micaArea = new ElaScrollPageArea(this);
    QHBoxLayout* micaLayout = new QHBoxLayout(micaArea);
    micaLayout->addWidget(_micaText);
    micaLayout->addStretch();
    micaLayout->addWidget(_normalButton);
    micaLayout->addWidget(_elaMicaButton);
#ifdef Q_OS_WIN
    micaLayout->addWidget(_micaButton);
    micaLayout->addWidget(_micaAltButton);
    micaLayout->addWidget(_acrylicButton);
    micaLayout->addWidget(_dwmBlurButton);
#endif

    // 用户卡片开关
    _userCardSwitchButton = new ElaToggleSwitch(this);
    ElaScrollPageArea* userCardArea = new ElaScrollPageArea(this);
    QHBoxLayout* userCardLayout = new QHBoxLayout(userCardArea);
    _userCardText = new ElaText(tr("Show User Card"), this);
    _userCardText->setWordWrap(false);
    _userCardText->setTextPixelSize(15);
    userCardLayout->addWidget(_userCardText);
    userCardLayout->addStretch();
    userCardLayout->addWidget(_userCardSwitchButton);
    connect(_userCardSwitchButton, &ElaToggleSwitch::toggled, this, [=](bool checked) {
        if (window)
            window->setUserInfoCardVisible(checked);
    });

    // ── 导航栏显示模式 ──────────────────────
    _minimumButton = new ElaRadioButton(tr("Minimal"), this);
    _compactButton = new ElaRadioButton(tr("Compact"), this);
    _maximumButton = new ElaRadioButton(tr("Maximal"), this);
    _autoButton    = new ElaRadioButton(tr("Auto"), this);
    _autoButton->setChecked(true);
    ElaScrollPageArea* navModeArea = new ElaScrollPageArea(this);
    QHBoxLayout* navModeLayout = new QHBoxLayout(navModeArea);
    _navModeText = new ElaText(tr("Navigation Bar Mode"), this);
    _navModeText->setWordWrap(false);
    _navModeText->setTextPixelSize(15);
    navModeLayout->addWidget(_navModeText);
    navModeLayout->addStretch();
    navModeLayout->addWidget(_minimumButton);
    navModeLayout->addWidget(_compactButton);
    navModeLayout->addWidget(_maximumButton);
    navModeLayout->addWidget(_autoButton);

    QButtonGroup* navGroup = new QButtonGroup(this);
    navGroup->addButton(_autoButton,    static_cast<int>(ElaNavigationType::Auto));
    navGroup->addButton(_minimumButton, static_cast<int>(ElaNavigationType::Minimal));
    navGroup->addButton(_compactButton, static_cast<int>(ElaNavigationType::Compact));
    navGroup->addButton(_maximumButton, static_cast<int>(ElaNavigationType::Maximal));
    connect(navGroup, QOverload<QAbstractButton*, bool>::of(&QButtonGroup::buttonToggled),
            this, [=](QAbstractButton* button, bool isToggled) {
        if (isToggled && window)
            window->setNavigationBarDisplayMode(
                static_cast<ElaNavigationType::NavigationDisplayMode>(navGroup->id(button)));
    });

    // ── 页面切换模式 ────────────────────────────────
    _noneButton   = new ElaRadioButton(tr("None"), this);
    _popupButton  = new ElaRadioButton(tr("Popup"), this);
    _popupButton->setChecked(true);
    _scaleButton  = new ElaRadioButton(tr("Scale"), this);
    _flipButton   = new ElaRadioButton(tr("Flip"), this);
    _blurButton   = new ElaRadioButton(tr("Blur"), this);
    ElaScrollPageArea* stackArea = new ElaScrollPageArea(this);
    QHBoxLayout* stackLayout = new QHBoxLayout(stackArea);
    _stackText = new ElaText(tr("Stack Switch Mode"), this);
    _stackText->setWordWrap(false);
    _stackText->setTextPixelSize(15);
    stackLayout->addWidget(_stackText);
    stackLayout->addStretch();
    stackLayout->addWidget(_noneButton);
    stackLayout->addWidget(_popupButton);
    stackLayout->addWidget(_scaleButton);
    stackLayout->addWidget(_flipButton);
    stackLayout->addWidget(_blurButton);

    QButtonGroup* stackGroup = new QButtonGroup(this);
    stackGroup->addButton(_noneButton,  0);
    stackGroup->addButton(_popupButton, 1);
    stackGroup->addButton(_scaleButton, 2);
    stackGroup->addButton(_flipButton,  3);
    stackGroup->addButton(_blurButton,  4);
    connect(stackGroup, QOverload<QAbstractButton*, bool>::of(&QButtonGroup::buttonToggled),
            this, [=](QAbstractButton* button, bool isToggled) {
        if (isToggled && window)
            window->setStackSwitchMode(
                static_cast<ElaWindowType::StackSwitchMode>(stackGroup->id(button)));
    });

    // ── 布局 ───────────────────────────────────────────
    _centralWidget = new QWidget(this);
    _centralWidget->setWindowTitle(tr("Settings"));
    QVBoxLayout* centerLayout = new QVBoxLayout(_centralWidget);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->addSpacing(30);
    centerLayout->addWidget(_langSectionText);
    centerLayout->addSpacing(10);
    centerLayout->addWidget(langArea);
    centerLayout->addSpacing(15);
    centerLayout->addWidget(_themeSectionText);
    centerLayout->addSpacing(10);
    centerLayout->addWidget(themeSwitchArea);
    centerLayout->addSpacing(15);
    centerLayout->addWidget(_appSectionText);
    centerLayout->addSpacing(10);
    centerLayout->addWidget(windowPaintArea);
    centerLayout->addWidget(micaArea);
    centerLayout->addWidget(userCardArea);
    centerLayout->addWidget(navModeArea);
    centerLayout->addWidget(stackArea);
    centerLayout->addStretch();
    addCentralWidget(_centralWidget, true, true, 0);
}

void SettingsPage::retranslateUi()
{
    setWindowTitle(tr("Settings"));
    if (_centralWidget) _centralWidget->setWindowTitle(tr("Settings"));

    // 段落标题
    if (_langSectionText) _langSectionText->setText(tr("Language"));
    if (_themeSectionText) _themeSectionText->setText(tr("Theme"));
    if (_appSectionText) _appSectionText->setText(tr("Application"));

    // 标签
    if (_langLabel) _langLabel->setText(tr("Language"));
    if (_themeSwitchText) _themeSwitchText->setText(tr("Theme Mode"));
    if (_windowPaintText) _windowPaintText->setText(tr("Window Paint Mode"));
    if (_micaText) _micaText->setText(tr("Window Effect"));
    if (_userCardText) _userCardText->setText(tr("Show User Card"));
    if (_navModeText) _navModeText->setText(tr("Navigation Bar Mode"));
    if (_stackText) _stackText->setText(tr("Stack Switch Mode"));

    // 语言下拉框 — 保留选中项，阻塞信号避免重复触发
    if (_languageComboBox) {
        _languageComboBox->blockSignals(true);
        QString curData = _languageComboBox->currentData().toString();
        _languageComboBox->setItemText(0, tr("English"));
        _languageComboBox->setItemText(1, tr("简体中文"));
        // 恢复选中项
        for (int i = 0; i < _languageComboBox->count(); ++i) {
            if (_languageComboBox->itemData(i).toString() == curData) {
                _languageComboBox->setCurrentIndex(i);
                break;
            }
        }
        _languageComboBox->blockSignals(false);
    }

    // 主题下拉框
    if (_themeComboBox) {
        _themeComboBox->blockSignals(true);
        int curThemeIdx = _themeComboBox->currentIndex();
        _themeComboBox->setItemText(0, tr("Light"));
        _themeComboBox->setItemText(1, tr("Dark"));
        _themeComboBox->setItemText(2, tr("Auto (System)"));
        _themeComboBox->setCurrentIndex(curThemeIdx);
        _themeComboBox->blockSignals(false);
    }

    // 窗口绘制模式单选按钮
    if (_windowNormalButton) _windowNormalButton->setText(tr("Normal"));
    if (_windowPixmapButton) _windowPixmapButton->setText(tr("Pixmap"));
    if (_windowMovieButton)  _windowMovieButton->setText(tr("Movie"));

    // 窗口特效单选按钮
    if (_normalButton)   _normalButton->setText(tr("Normal"));
    if (_elaMicaButton)  _elaMicaButton->setText(tr("ElaMica"));
#ifdef Q_OS_WIN
    if (_micaButton)     _micaButton->setText(tr("Mica"));
    if (_micaAltButton)  _micaAltButton->setText(tr("Mica-Alt"));
    if (_acrylicButton)  _acrylicButton->setText(tr("Acrylic"));
    if (_dwmBlurButton)  _dwmBlurButton->setText(tr("Dwm-Blur"));
#endif

    // 导航栏模式单选按钮
    if (_minimumButton) _minimumButton->setText(tr("Minimal"));
    if (_compactButton) _compactButton->setText(tr("Compact"));
    if (_maximumButton) _maximumButton->setText(tr("Maximal"));
    if (_autoButton)    _autoButton->setText(tr("Auto"));

    // 页面切换模式单选按钮
    if (_noneButton)   _noneButton->setText(tr("None"));
    if (_popupButton)  _popupButton->setText(tr("Popup"));
    if (_scaleButton)  _scaleButton->setText(tr("Scale"));
    if (_flipButton)   _flipButton->setText(tr("Flip"));
    if (_blurButton)   _blurButton->setText(tr("Blur"));
}

SettingsPage::~SettingsPage()
{
#ifdef Q_OS_WIN
    ThemeChangeWatcher::instance().setAutoPage(nullptr);
#endif
}
