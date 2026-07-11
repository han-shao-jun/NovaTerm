#include "LanguageManager.h"
#include <QCoreApplication>
#include <QDebug>

LanguageManager& LanguageManager::instance()
{
    static LanguageManager mgr;
    return mgr;
}

void LanguageManager::install(const QString& preferredLocale)
{
    loadTranslations();

    // 选择启动语言：首选语言 > zh_CN > en
    if (!preferredLocale.isEmpty() && _translators.contains(preferredLocale)) {
        _currentLocale = preferredLocale;
    } else if (_translators.contains("zh_CN")) {
        _currentLocale = "zh_CN";
    } else {
        _currentLocale = "en";
    }

    // 安装当前翻译器并发射信号
    auto* t = _translators.value(_currentLocale);
    if (t) {
        QCoreApplication::installTranslator(t);
        emit languageChanged(_currentLocale);
    }
}

void LanguageManager::switchLanguage(const QString& locale)
{
    if (_currentLocale == locale || !_translators.contains(locale))
        return;

    // 移除旧翻译器
    auto* old = _translators.value(_currentLocale);
    if (old)
        QCoreApplication::removeTranslator(old);

    // 安装新翻译器
    _currentLocale = locale;
    auto* t = _translators.value(_currentLocale);
    if (t) {
        QCoreApplication::installTranslator(t);
        qDebug() << "已切换语言至：" << locale;
    }

    emit languageChanged(_currentLocale);
}

QStringList LanguageManager::availableLocales() const
{
    return _translators.keys();
}

void LanguageManager::loadTranslations()
{
    // 从 Qt 资源文件（通过 .qrc 嵌入）加载 .qm
    struct LocaleInfo { QString name; QString label; };
    const QList<LocaleInfo> locales = {
        {"en",     "English"},
        {"zh_CN",  "简体中文"},
    };

    for (const auto& loc : locales) {
        QTranslator* t = new QTranslator(this);
        // 使用完整资源路径并显式指定 .qm 后缀
        QString qmPath = ":/translations/novaterm_" + loc.name + ".qm";
        if (t->load(qmPath)) {
            _translators.insert(loc.name, t);
            qDebug() << "已加载翻译：" << qmPath;
        } else {
            qWarning() << "加载翻译失败：" << qmPath;
            delete t;
        }
    }
}
