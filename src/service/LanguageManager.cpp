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

    // 安装当前语言整套翻译器（qtbase + 应用）并发射信号
    applyLocale(_currentLocale);
    emit languageChanged(_currentLocale);
}

void LanguageManager::switchLanguage(const QString& locale)
{
    if (_currentLocale == locale || !_translators.contains(locale))
        return;

    applyLocale(locale);
    qDebug() << "已切换语言至：" << locale;
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
        QString qmPath = ":/i18n/novaterm_" + loc.name + ".qm";
        if (t->load(qmPath)) {
            _translators.insert(loc.name, t);
            qDebug() << "已加载翻译：" << qmPath;
        } else {
            qWarning() << "加载翻译失败：" << qmPath;
            delete t;
        }
    }
}

void LanguageManager::applyLocale(const QString& locale)
{
    // 先移除当前整套翻译器（qtbase + 应用），再安装新语言的整套。
    if (_qtbaseTranslator) {
        QCoreApplication::removeTranslator(_qtbaseTranslator);
        _qtbaseTranslator = nullptr;
    }
    if (QTranslator* old = _translators.value(_currentLocale))
        QCoreApplication::removeTranslator(old);
    _currentLocale = locale;

    // 1) Qt 基础翻译（QFileDialog / QMessageBox / QLineEdit 菜单等标准控件文案）。
    //    文件在 exe 同级的 translations/ 目录，由构建/安装规则从 Qt 安装目录复制。
    //    找不到（例如仅 en 语言或纯开发环境）时静默跳过 —— 应用译文不受影响。
    if (locale != QStringLiteral("en")) {
        auto* base = new QTranslator(this);
        const QString basePath = QCoreApplication::applicationDirPath()
            + QStringLiteral("/translations/qtbase_")
            + locale + QStringLiteral(".qm");
        if (base->load(basePath)) {
            // 先装 qtbase、后装应用翻译器：Qt 按安装逆序查找，因此应用译文优先。
            QCoreApplication::installTranslator(base);
            _qtbaseTranslator = base;
        } else {
            delete base;
        }
    }

    // 2) 应用翻译器（内嵌资源，含本工程全部 tr() 文案）
    if (QTranslator* t = _translators.value(locale))
        QCoreApplication::installTranslator(t);
}
