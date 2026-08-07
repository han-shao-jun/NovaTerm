#include "SshHostKeyDialog.h"

#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

SshHostKeyDialog::SshHostKeyDialog(const SshHostKeyInfo& info, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Confirm SSH Host Key"));
    setModal(true);
    setMinimumWidth(520);

    const bool changed = info.status == SshHostKeyStatus::Changed;

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 16);
    layout->setSpacing(12);

    // 标题 + 警告图标
    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(10);
    QIcon warningIcon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(warningIcon.pixmap(32, 32));
    iconLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    titleRow->addWidget(iconLabel);

    auto* titleLabel = new QLabel(this);
    titleLabel->setWordWrap(true);
    titleLabel->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 14px;"));
    titleLabel->setText(changed
        ? tr("Warning: the host key for %1:%2 has changed!")
        : tr("The authenticity of host %1:%2 cannot be established.")
              .arg(info.host)
              .arg(info.port));
    titleRow->addWidget(titleLabel, 1);
    layout->addLayout(titleRow);

    if (changed) {
        auto* changeNote = new QLabel(
            tr("This may indicate a man-in-the-middle attack. Do not continue "
               "unless you have a reason to expect this change."),
            this);
        changeNote->setWordWrap(true);
        layout->addWidget(changeNote);
    }

    // 密钥详情
    auto* detailsFrame = new QFrame(this);
    detailsFrame->setFrameShape(QFrame::StyledPanel);
    auto* detailsLayout = new QGridLayout(detailsFrame);
    detailsLayout->setContentsMargins(12, 10, 12, 10);
    detailsLayout->setHorizontalSpacing(12);
    detailsLayout->setVerticalSpacing(6);

    const auto addRow = [detailsLayout, this](const QString& key,
                                              const QString& value,
                                              bool mono = false) {
        auto* keyLabel = new QLabel(key, this);
        keyLabel->setStyleSheet(QStringLiteral("color: gray;"));
        detailsLayout->addWidget(keyLabel, detailsLayout->rowCount(), 0);
        auto* valueLabel = new QLabel(value, this);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (mono)
            valueLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        detailsLayout->addWidget(valueLabel, detailsLayout->rowCount() - 1, 1);
    };

    addRow(tr("Server:"), QStringLiteral("%1:%2").arg(info.host).arg(info.port));
    addRow(tr("Key type:"), info.keyType);
    addRow(tr("Fingerprint (SHA-256):"), info.fingerprint, true);

    layout->addWidget(detailsFrame);

    // 按钮行
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch();

    auto* rejectButton = new QPushButton(tr("Reject"), this);
    rejectButton->setDefault(false);
    buttonRow->addWidget(rejectButton);

    auto* acceptButton =
        new QPushButton(changed ? tr("Update and Connect")
                                : tr("Trust and Connect"),
                        this);
    acceptButton->setDefault(true);
    buttonRow->addWidget(acceptButton);

    layout->addLayout(buttonRow);

    connect(rejectButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(acceptButton, &QPushButton::clicked, this, &QDialog::accept);
}
