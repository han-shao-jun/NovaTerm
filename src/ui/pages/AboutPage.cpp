/**
 * @file   AboutPage.cpp
 * @brief  关于页面实现：版本信息与多语言文本构建。
 */
#include "AboutPage.h"
#include "ElaText.h"
#include "service/LanguageManager.h"
#include <QCoreApplication>
#include <QVBoxLayout>

AboutPage::AboutPage(QWidget* parent) : ElaScrollPage(parent)
{
    // 所有可见文本统一在 retranslateUi() 中通过 tr() 设置，构造期不单独赋文案。
    // 此前描述文本在构造与 retranslateUi 中各写一份且内容不一致（Qt 6.7 +
    // QTermWidget 的旧版本残留在 retranslateUi），语言切换一次内容就发生跳变。
    _centralWidget = new QWidget(this);
    auto* layout = new QVBoxLayout(_centralWidget);
    layout->setContentsMargins(60, 80, 60, 80);
    layout->setSpacing(12);

    _titleText = new ElaText(this);
    _titleText->setTextPixelSize(28);
    layout->addWidget(_titleText);

    _versionText = new ElaText(this);
    _versionText->setTextPixelSize(15);
    layout->addWidget(_versionText);

    layout->addSpacing(20);

    _descText = new ElaText(this);
    _descText->setTextPixelSize(13);
    layout->addWidget(_descText);

    layout->addSpacing(20);

    _licenseText = new ElaText(this);
    _licenseText->setTextPixelSize(12);
    layout->addWidget(_licenseText);

    layout->addStretch();

    addCentralWidget(_centralWidget);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });

    retranslateUi();
}

void AboutPage::retranslateUi()
{
    setWindowTitle(tr("About"));
    if (_centralWidget) _centralWidget->setWindowTitle(tr("About"));
    if (_titleText) _titleText->setText(tr("NovaTerm"));
    if (_versionText) {
        _versionText->setText(
            tr("Version %1").arg(QCoreApplication::applicationVersion()));
    }
    if (_descText) _descText->setText(
        tr("A cross-platform terminal emulator & SSH client\n"
           "with FluentUI design, inspired by WindTerm.\n\n"
           "Built with:\n"
           "  • Qt 6.8  •  ElaWidgetTools (FluentUI)\n"
           "  • libvterm-0.3.3 (terminal emulation)\n"
           "  • libssh (SSH/SFTP)"));
    if (_licenseText) _licenseText->setText(tr("License: GPLv2+"));
}
