#pragma once
#include "ElaScrollPage.h"
#include "ElaTheme.h"

class ElaRadioButton;
class ElaToggleSwitch;
class ElaComboBox;
class ElaText;

// 设置视图（ElaScrollPage）。管理语言、主题（含"自动 = 跟随系统"），
// 以及宿主 ElaWindow 的外观（绘制模式、窗口特效、导航栏模式、页面切换动画）。
// 窗口相关控件作用于作为父对象传入的 ElaWindow，因此本页面在对话框内显示时
// 仍然可用。通过 Q_INVOKABLE 构造，可被反射式创建。
class SettingsPage : public ElaScrollPage
{
    Q_OBJECT
public:
    Q_INVOKABLE explicit SettingsPage(QWidget* parent = nullptr);
    ~SettingsPage() override;

    void applyAutoTheme();

    // 主题 — 跟随系统（在全局启动时使用）
    static bool isSystemDarkTheme();

    // 由程序化主题变更（自动/跟随系统）设置，置 true 后
    // themeModeChanged 处理函数将跳过，防止覆盖用户显式选择。
    static bool s_themeProgrammaticChange;

private:
    void retranslateUi();

    void onThemeComboChanged(int index);

    ElaComboBox* _themeComboBox{nullptr};
    int _themeAutoComboIndex = 2; // 下拉框中 "Auto (System)" 的索引
    bool _themeAutoMode = true;

    // 窗口绘制模式
    ElaRadioButton* _windowNormalButton{nullptr};
    ElaRadioButton* _windowPixmapButton{nullptr};
    ElaRadioButton* _windowMovieButton{nullptr};

    // 窗口特效
    ElaRadioButton* _normalButton{nullptr};
    ElaRadioButton* _elaMicaButton{nullptr};
#ifdef Q_OS_WIN
    ElaRadioButton* _micaButton{nullptr};
    ElaRadioButton* _micaAltButton{nullptr};
    ElaRadioButton* _acrylicButton{nullptr};
    ElaRadioButton* _dwmBlurButton{nullptr};
#endif

    // 用户卡片
    ElaToggleSwitch* _userCardSwitchButton{nullptr};

    // 导航栏显示模式
    ElaRadioButton* _minimumButton{nullptr};
    ElaRadioButton* _compactButton{nullptr};
    ElaRadioButton* _maximumButton{nullptr};
    ElaRadioButton* _autoButton{nullptr};

    // 页面切换模式
    ElaRadioButton* _noneButton{nullptr};
    ElaRadioButton* _popupButton{nullptr};
    ElaRadioButton* _scaleButton{nullptr};
    ElaRadioButton* _flipButton{nullptr};
    ElaRadioButton* _blurButton{nullptr};

    // 语言
    ElaComboBox* _languageComboBox{nullptr};

    // ── 可翻译的标签控件 ──────────────────────────
    ElaText* _langSectionText{nullptr};
    ElaText* _langLabel{nullptr};
    ElaText* _themeSectionText{nullptr};
    ElaText* _themeSwitchText{nullptr};
    ElaText* _appSectionText{nullptr};
    ElaText* _windowPaintText{nullptr};
    ElaText* _micaText{nullptr};
    ElaText* _userCardText{nullptr};
    ElaText* _navModeText{nullptr};
    ElaText* _stackText{nullptr};
    QWidget* _centralWidget{nullptr};
};

// Windows 原生事件过滤器 — 监听 WM_SETTINGCHANGE（InmmersiveColorSet）
// 以检测系统明暗主题切换，全局有效，不依赖 SettingsPage 实例。
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
class ThemeChangeWatcher : public QAbstractNativeEventFilter
{
public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;
#else
    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override;
#endif
};
#endif
