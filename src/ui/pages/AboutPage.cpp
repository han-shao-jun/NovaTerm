#include "AboutPage.h"
#include "ElaText.h"
#include "core/LanguageManager.h"
#include <QVBoxLayout>

AboutPage::AboutPage(QWidget* parent) : ElaScrollPage(parent)
{
    setWindowTitle(tr("About"));

    _centralWidget = new QWidget(this);
    _centralWidget->setWindowTitle(tr("About"));
    auto* layout = new QVBoxLayout(_centralWidget);
    layout->setContentsMargins(60, 80, 60, 80);
    layout->setSpacing(12);

    _titleText = new ElaText(tr("NovaTerm"), this);
    _titleText->setTextPixelSize(28);
    layout->addWidget(_titleText);

    _versionText = new ElaText(tr("Version 0.1.0"), this);
    _versionText->setTextPixelSize(15);
    layout->addWidget(_versionText);

    layout->addSpacing(20);

    _descText = new ElaText(
        tr("A cross-platform terminal emulator & SSH client\n"
           "with FluentUI design, inspired by WindTerm.\n\n"
           "Built with:\n"
           "  • Qt 6.7  •  ElaWidgetTools (FluentUI)\n"
           "  • QTermWidget (terminal emulation)\n"
           "  • libssh (SSH/SFTP)"),
        this);
    _descText->setTextPixelSize(13);
    layout->addWidget(_descText);

    layout->addSpacing(20);

    _licenseText = new ElaText(tr("License: GPLv2+"), this);
    _licenseText->setTextPixelSize(12);
    layout->addWidget(_licenseText);

    layout->addStretch();

    addCentralWidget(_centralWidget);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void AboutPage::retranslateUi()
{
    setWindowTitle(tr("About"));
    if (_centralWidget) _centralWidget->setWindowTitle(tr("About"));
    if (_titleText) _titleText->setText(tr("NovaTerm"));
    if (_versionText) _versionText->setText(tr("Version 0.1.0"));
    if (_descText) _descText->setText(
        tr("A cross-platform terminal emulator & SSH client\n"
           "with FluentUI design, inspired by WindTerm.\n\n"
           "Built with:\n"
           "  • Qt 6.7  •  ElaWidgetTools (FluentUI)\n"
           "  • QTermWidget (terminal emulation)\n"
           "  • libssh (SSH/SFTP)"));
    if (_licenseText) _licenseText->setText(tr("License: GPLv2+"));
}
