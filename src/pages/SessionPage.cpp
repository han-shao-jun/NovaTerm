#include "SessionPage.h"
#include "ElaComboBox.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "core/LanguageManager.h"
#include "ElaTabWidget.h"
#include <QTabWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>

SessionPage::SessionPage(QWidget* parent)
    : ElaScrollPage(parent)
{
    setWindowTitle(tr("Session"));

    // ── 主容器 ────────────────────────────────────────
    auto* centralWidget = new QWidget(this);

    // 外层垂直布局：splitLayout + btnLayout
    auto* centralLayout = new QVBoxLayout(centralWidget);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    // ── 左侧选项卡 + 右侧堆栈页面 ──────────────────
    _tabWidget = new VerticalTabWidget(centralWidget);

    centralLayout->addWidget(_tabWidget, 1);

    // ── 右侧 Stacked 页面：本地 Shell 配置 ──────────────
    auto* page = new QWidget(centralWidget);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    // Type 选择行
    auto* typeLayout = new QHBoxLayout();
    typeLayout->setSpacing(8);

    // ── 本地 Shell：Shell 类型选择（cmd / PowerShell）──
    _localTypeLabel = new ElaText(tr("Type"), page);
    _localTypeLabel->setWordWrap(false);
    _localTypeLabel->setTextPixelSize(15);
    typeLayout->addWidget(_localTypeLabel);

    _localShellTypeCombo = new ElaComboBox(page);
    _localShellTypeCombo->addItem(QStringLiteral("cmd"));
    _localShellTypeCombo->addItem(QStringLiteral("PowerShell"));
    _localShellTypeCombo->setMinimumWidth(160);
    typeLayout->addWidget(_localShellTypeCombo);
    typeLayout->addStretch();

    pageLayout->addLayout(typeLayout);

    // Label 行
    auto* labelLayout = new QHBoxLayout();
    labelLayout->setSpacing(8);
    _localLabel = new ElaText(tr("Label"), page);
    _localLabel->setWordWrap(false);
    _localLabel->setTextPixelSize(15);
    labelLayout->addWidget(_localLabel);
    labelLayout->addStretch();

    pageLayout->addLayout(labelLayout);
    pageLayout->addStretch();

    _tabWidget->addTab(page, tr("local shell"));

    // 占位页面（后续实现）
    _tabWidget->addTab(new QWidget(centralWidget), tr("ssh"));
    _tabWidget->addTab(new QWidget(centralWidget), tr("serial port"));
    _tabWidget->addTab(new QWidget(centralWidget), tr("telnet"));

    // ── 底部按钮行（靠右）──────────────────────────────
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(12, 8, 12, 8);
    btnLayout->setSpacing(8);
    btnLayout->addStretch();

    auto* cancelBtn = new ElaPushButton(tr("Cancel"), centralWidget);
    btnLayout->addWidget(cancelBtn);

    auto* confirmBtn = new ElaPushButton(tr("Confirm"), centralWidget);
    btnLayout->addWidget(confirmBtn);

    centralLayout->addLayout(btnLayout);

    // ── 信号连接 ──
    connect(cancelBtn, &QPushButton::clicked,
            this, &SessionPage::dialogRejected);
    connect(confirmBtn, &QPushButton::clicked, this, [this]() {
        const auto type =
            (_localShellTypeCombo->currentText() == QStringLiteral("PowerShell"))
                ? TerminalView::LocalShellType::PowerShell
                : TerminalView::LocalShellType::Cmd;
        emit localSessionRequested(type);
    });

    addCentralWidget(centralWidget, true, true, 0);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void SessionPage::retranslateUi()
{

}
