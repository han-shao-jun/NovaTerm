#include "SessionPage.h"
#include "ElaTabWidget.h"
#include "ElaTabBar.h"
#include "ElaText.h"
#include "ElaComboBox.h"
#include "ElaPushButton.h"
#include "ElaMessageBar.h"
#include "core/LanguageManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>

SessionPage::SessionPage(QWidget* parent)
    : ElaScrollPage(parent)
{
    setWindowTitle(tr("Session"));

    // ── 标签页控件 ──────────────────────────────────────
    auto* page = new QWidget(this);
    auto* pageLayout = new QVBoxLayout(page); //垂直布局
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    // ── 本地 Shell：Shell 类型选择（类型标签 + 下拉框，水平布局）──
    auto* typeLayout = new QHBoxLayout(); //水平布局
    typeLayout->setSpacing(8);

    _localTypeLabel = new ElaText(tr("Type"), page);
    _localTypeLabel->setWordWrap(false);
    _localTypeLabel->setTextPixelSize(15);
    typeLayout->addWidget(_localTypeLabel);

    _localShellTypeCombo = new ElaComboBox(page);
    _localShellTypeCombo->addItem(QStringLiteral("cmd"));
    _localShellTypeCombo->addItem(QStringLiteral("PowerShell"));
    _localShellTypeCombo->setMinimumWidth(160);
    typeLayout->addWidget(_localShellTypeCombo);
    typeLayout->addStretch(); //在右侧添加一个弹簧

    pageLayout->addLayout(typeLayout);
    pageLayout->addSpacing(12);

    auto* labelLayout = new QHBoxLayout(); //水平布局
    labelLayout->setSpacing(8);
    _localLabel = new ElaText(tr("Label"), page);
    _localLabel->setWordWrap(false);
    _localLabel->setTextPixelSize(15);
    labelLayout->addWidget(_localLabel);
    labelLayout->addStretch(); //在右侧添加一个弹簧

    pageLayout->addLayout(labelLayout);
    pageLayout->addSpacing(12);

    // ── 确定 + 取消（右下角）──
    auto* btnLayout = new QHBoxLayout(); //水平布局
    btnLayout->setSpacing(8);
    btnLayout->addStretch();

    auto* cancelBtn = new ElaPushButton(tr("Cancel"), page);
    btnLayout->addWidget(cancelBtn);

    auto* confirmBtn = new ElaPushButton(tr("Confirm"), page);
    btnLayout->addWidget(confirmBtn);

    pageLayout->addLayout(btnLayout);
    pageLayout->addStretch(); //在底部添加一个弹簧

    // ── 信号连接 ──
    connect(cancelBtn, &QPushButton::clicked,
            this, &SessionPage::dialogRejected);
    // 读取 Shell 类型下拉框，映射为枚举后发出。默认（含未来新增项）按 cmd 处理，
    // 即关联到 Clink。
    connect(confirmBtn, &QPushButton::clicked, this, [this]() {
        const auto type =
            (_localShellTypeCombo->currentText() == QStringLiteral("PowerShell"))
                ? TerminalView::LocalShellType::PowerShell
                : TerminalView::LocalShellType::Cmd;
        emit localSessionRequested(type);
    });

    addCentralWidget(page, true, true, 0);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void SessionPage::retranslateUi()
{

}
