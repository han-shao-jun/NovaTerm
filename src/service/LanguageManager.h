#pragma once
#include <QObject>
#include <QTranslator>
#include <QMap>

// 运行时语言切换器（单例）。预加载每个内置 .qm 翻译器，
// 然后实时切换当前翻译器。控件订阅 languageChanged 信号，
// 在各自的 retranslateUi() 中重新应用 tr() 字符串 —— 无需重启。
//
// 每个语言由两个翻译器组成，安装顺序固定（旧翻译器先查、先装后查）：
//   1. Qt 基础翻译（qtbase_<locale>.qm，来自 Qt 安装目录的 translations/，
//      部署时随 exe 复制到同级的 translations/ 子目录）：覆盖 QFileDialog、
//      QMessageBox、QLineEdit 右键菜单等标准控件内置文案；
//   2. 应用翻译（novaterm_<locale>.qm，qt_add_translations 内嵌进资源）：
//      覆盖本工程全部 tr() 文案。后安装的优先，因此工程译文可覆盖 Qt 默认。
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
    // 安装某语言的一整套翻译器（qtbase + 应用）。调用前须已移除旧整套。
    void applyLocale(const QString& locale);

    // 语言区域名 → 应用翻译器（qtbase 翻译器不在此表；可回退时为空）。
    QMap<QString, QTranslator*> _translators;
    // 当前已安装的 qtbase 翻译器；nullptr 表示该语言无 Qt 基础翻译。
    QTranslator* _qtbaseTranslator{nullptr};
    QString _currentLocale = "en";
};
