/**
 * @file   SshHostKeyDialog.cpp
 * @brief  主机密钥验证对话框实现：指纹展示与 Accept/Reject。
 */
#include "SshHostKeyDialog.h"

#include "ElaPushButton.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QVBoxLayout>

SshHostKeyDialog::SshHostKeyDialog(const SshHostKeyInfo& info, QWidget* parent)
    : ElaDialog(parent)
{
    setWindowTitle(tr("Confirm SSH Host Key"));
    setModal(true);
    setMinimumWidth(520);
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    setAppBarHeight(32);

    const bool changed = info.status == SshHostKeyStatus::Changed;

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 44, 24, 16);
    layout->setSpacing(12);

    // 标题 + 警告图标
    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(10);
    auto* iconLabel = new ElaText(this);
    iconLabel->setElaIcon(ElaIconType::TriangleExclamation);
    iconLabel->setTextPixelSize(28);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setFixedSize(32, 32);
    iconLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    titleRow->addWidget(iconLabel);

    const QString title = (changed
        ? tr("Warning: the host key for %1:%2 has changed!")
        : tr("The authenticity of host %1:%2 cannot be established."))
            .arg(info.host)
            .arg(info.port);
    auto* titleLabel = new ElaText(title, this);
    titleLabel->setTextStyle(ElaTextType::BodyStrong);
    titleLabel->setWordWrap(true);
    titleRow->addWidget(titleLabel, 1);
    layout->addLayout(titleRow);

    if (changed) {
        auto* changeNote = new ElaText(
            tr("This may indicate a man-in-the-middle attack. Do not continue "
               "unless you have a reason to expect this change."),
            this);
        changeNote->setTextStyle(ElaTextType::Body);
        changeNote->setWordWrap(true);
        layout->addWidget(changeNote);
    }

    // 密钥详情
    auto* detailsArea = new ElaScrollPageArea(this);
    auto* detailsLayout = new QGridLayout(detailsArea);
    detailsLayout->setContentsMargins(12, 10, 12, 10);
    detailsLayout->setHorizontalSpacing(12);
    detailsLayout->setVerticalSpacing(6);

    const auto addRow = [detailsLayout, detailsArea](const QString& key,
                                                     const QString& value,
                                                     bool mono = false) {
        auto* keyLabel = new ElaText(key, detailsArea);
        keyLabel->setTextStyle(ElaTextType::Caption);
        detailsLayout->addWidget(keyLabel, detailsLayout->rowCount(), 0);
        auto* valueLabel = new ElaText(value, detailsArea);
        valueLabel->setTextStyle(ElaTextType::Body);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (mono)
            valueLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        detailsLayout->addWidget(valueLabel, detailsLayout->rowCount() - 1, 1);
    };

    addRow(tr("Server:"), QStringLiteral("%1:%2").arg(info.host).arg(info.port));
    addRow(tr("Key type:"), info.keyType);
    addRow(tr("Fingerprint (SHA-256):"), info.fingerprint, true);

    layout->addWidget(detailsArea);

    // 按钮行
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();

    auto* rejectButton = new ElaPushButton(tr("Reject"), this);
    rejectButton->setMinimumSize(88, 36);
    rejectButton->setDefault(false);
    buttonRow->addWidget(rejectButton);

    auto* acceptButton =
        new ElaPushButton(changed ? tr("Update and Connect")
                                  : tr("Trust and Connect"),
                          this);
    acceptButton->setMinimumSize(120, 36);
    acceptButton->setDefault(true);
    acceptButton->setLightDefaultColor(
        ElaThemeColor(ElaThemeType::Light, PrimaryNormal));
    acceptButton->setLightHoverColor(
        ElaThemeColor(ElaThemeType::Light, PrimaryHover));
    acceptButton->setLightPressColor(
        ElaThemeColor(ElaThemeType::Light, PrimaryPress));
    acceptButton->setLightTextColor(Qt::white);
    acceptButton->setDarkDefaultColor(
        ElaThemeColor(ElaThemeType::Dark, PrimaryNormal));
    acceptButton->setDarkHoverColor(
        ElaThemeColor(ElaThemeType::Dark, PrimaryHover));
    acceptButton->setDarkPressColor(
        ElaThemeColor(ElaThemeType::Dark, PrimaryPress));
    acceptButton->setDarkTextColor(Qt::black);
    buttonRow->addWidget(acceptButton);

    layout->addLayout(buttonRow);

    connect(rejectButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
}
