/**
 * @file   LanguageManager.h
 * @brief  运行时语言切换器（单例）。
 *
 * 预加载每个内置 .qm 翻译器，运行时实时切换当前翻译器。
 * 控件订阅 languageChanged 信号，在各自的 retranslateUi() 中重新应用
 * tr() 字符串 —— 无需重启。
 */
#pragma once
#include <QObject>
#include <QTranslator>
#include <QMap>

// 运行时语言切换器（单例）。预加载每个内置 .qm 翻译器，
// 然后实时切换当前翻译器。控件订阅 languageChanged 信号，
// 在各自的 retranslateUi() 中重新应用 tr() 字符串 —— 无需重启。
class LanguageManager : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 获取单例实例。
     * @return 语言管理器单例引用。
     */
    static LanguageManager& instance();

    /**
     * @brief 应用首选语言。
     * @param preferredLocale 首选语言区域名；为空或不可用时依次回退到 "zh_CN"、"en"。
     */
    void install(const QString& preferredLocale = QString());

    /**
     * @brief 运行时切换翻译器。
     * @param locale 语言区域名（"en" 或 "zh_CN"）。
     * @note 成功时发射 languageChanged 信号。
     */
    void switchLanguage(const QString& locale);

    /**
     * @brief 获取当前语言区域名。
     * @return 当前 locale。
     */
    QString currentLocale() const { return _currentLocale; }

    /**
     * @brief 列出所有已加载翻译器的语言区域。
     * @return 可用 locale 列表。
     */
    QStringList availableLocales() const;

signals:
    /**
     * @brief 翻译器已变更。
     * @param locale 新的语言区域名。
     * @note 连接到槽函数中重新应用所有 tr() 文本；
     *       QEvent::LanguageChange 不会自动发送。
     */
    void languageChanged(const QString& locale);

private:
    LanguageManager() = default;
    void loadTranslations();

    QMap<QString, QTranslator*> _translators;  // 语言区域名 → 已加载的翻译器
    QString _currentLocale = "en";
};
