#include "SessionPage.h"
#include "ElaTabWidget.h"
#include "ElaTabBar.h"
#include "ElaText.h"
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
    _tabWidget = new ElaTabWidget(this);
    _tabWidget->setTabPosition(QTabWidget::North);
    _tabWidget->setIndicatorPosition(ElaTabBarType::Bottom);
    _tabWidget->setDocumentMode(true);
    _tabWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    _tabWidget->tabBar()->setTabsClosable(false);
    _tabWidget->tabBar()->setExpanding(true);

    // 四个 Tab：本地终端、SSH、串口、Telnet
    const QList<QPair<QString, bool>> tabs = {
        {tr("Local Shell"), true},
        {tr("SSH"),         false},
        {tr("Serial"),      false},
        {tr("Telnet"),      false},
    };

    // 收集各标签页的占位文本和按钮指针
    QList<ElaText**> placeholders = {
        &_localPlaceholder, &_sshPlaceholder,
        &_serialPlaceholder, &_telnetPlaceholder
    };
    QList<ElaPushButton**> confirmBtns = {
        &_localConfirmBtn, &_sshConfirmBtn,
        &_serialConfirmBtn, &_telnetConfirmBtn
    };
    QList<ElaPushButton**> cancelBtns = {
        &_localCancelBtn, &_sshCancelBtn,
        &_serialCancelBtn, &_telnetCancelBtn
    };

    for (int i = 0; i < tabs.size(); ++i) {
        const auto& [title, isLocal] = tabs[i];

        auto* page = new QWidget(_tabWidget);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(24, 28, 24, 20);
        pageLayout->setSpacing(0);

        // ── 居中占位文本 ──
        pageLayout->addStretch();
        auto* placeholder = new ElaText(
            isLocal
                ? tr("Start a local terminal session.\n"
                     "Full terminal emulation is provided by\n"
                     "ConPTY (Windows) or PTY (Unix).")
                : tr("This session type will be available\n"
                     "in a future update."),
            page);
        placeholder->setTextPixelSize(14);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setWordWrap(true);
        pageLayout->addWidget(placeholder, 0, Qt::AlignCenter);
        pageLayout->addStretch();

        // ── 确定 + 取消（右下角）──
        auto* btnLayout = new QHBoxLayout();
        btnLayout->setSpacing(8);
        btnLayout->addStretch();

        auto* cancelBtn = new ElaPushButton(tr("Cancel"), page);
        btnLayout->addWidget(cancelBtn);

        auto* confirmBtn = new ElaPushButton(tr("Confirm"), page);
        btnLayout->addWidget(confirmBtn);

        pageLayout->addLayout(btnLayout);

        // 保存指针
        *placeholders[i] = placeholder;
        *cancelBtns[i] = cancelBtn;
        *confirmBtns[i] = confirmBtn;

        // ── 信号连接 ──
        connect(cancelBtn, &QPushButton::clicked,
                this, &SessionPage::dialogRejected);

        if (isLocal) {
            connect(confirmBtn, &QPushButton::clicked,
                    this, &SessionPage::localSessionRequested);
        } else {
            connect(confirmBtn, &QPushButton::clicked, this, [this]() {
                ElaMessageBar::information(ElaMessageBarType::BottomRight,
                                           tr("Not implemented"),
                                           tr("Not implemented yet"), 2000);
            });
        }

        _tabWidget->addTab(page, title);
    }

    // ── 中央布局 ────────────────────────────────────────
    _centralWidget = new QWidget(this);
    _centralWidget->setWindowTitle(tr("Session"));
    auto* centerLayout = new QVBoxLayout(_centralWidget);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->addWidget(_tabWidget);
    addCentralWidget(_centralWidget, true, true, 0);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void SessionPage::retranslateUi()
{
    setWindowTitle(tr("Session"));
    if (_centralWidget)
        _centralWidget->setWindowTitle(tr("Session"));

    // 更新标签页标题
    if (_tabWidget) {
        _tabWidget->setTabText(0, tr("Local Shell"));
        _tabWidget->setTabText(1, tr("SSH"));
        _tabWidget->setTabText(2, tr("Serial"));
        _tabWidget->setTabText(3, tr("Telnet"));
    }

    // 更新占位文本
    if (_localPlaceholder)
        _localPlaceholder->setText(
            tr("Start a local terminal session.\n"
               "Full terminal emulation is provided by\n"
               "ConPTY (Windows) or PTY (Unix)."));
    auto updateOther = [this](ElaText* w) {
        if (w) w->setText(tr("This session type will be available\n"
                             "in a future update."));
    };
    updateOther(_sshPlaceholder);
    updateOther(_serialPlaceholder);
    updateOther(_telnetPlaceholder);

    // 更新按钮文本
    auto updateBtns = [this](ElaPushButton* confirm, ElaPushButton* cancel) {
        if (confirm) confirm->setText(tr("Confirm"));
        if (cancel)  cancel->setText(tr("Cancel"));
    };
    updateBtns(_localConfirmBtn,  _localCancelBtn);
    updateBtns(_sshConfirmBtn,    _sshCancelBtn);
    updateBtns(_serialConfirmBtn, _serialCancelBtn);
    updateBtns(_telnetConfirmBtn, _telnetCancelBtn);
}
