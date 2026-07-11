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
    static LanguageManager& instance();

    // 应用首选语言。若不可用则依次回退到 "zh_CN"、"en"。
    void install(const QString& preferredLocale = QString());
    // 运行时切换翻译器；成功时发射 languageChanged。
    void switchLanguage(const QString& locale);  // "en" 或 "zh_CN"
    QString currentLocale() const { return _currentLocale; }
    QStringList availableLocales() const;

signals:
    // 在翻译器变更后发射。连接到槽函数中重新应用所有 tr() 文本；
    // QEvent::LanguageChange 不会自动发送。
    void languageChanged(const QString& locale);

private:
    LanguageManager() = default;
    void loadTranslations();

    QMap<QString, QTranslator*> _translators;  // 语言区域名 → 已加载的翻译器
    QString _currentLocale = "en";
};
