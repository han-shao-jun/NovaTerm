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
    _centralWidget = new QWidget(this);

    // 外层垂直布局：splitLayout + btnLayout
    auto* centralLayout = new QVBoxLayout(_centralWidget); // 垂直布局
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    // ── 左侧选项卡 + 右侧堆栈页面 ──────────────────
    _tabWidget = new VerticalTabWidget(_centralWidget);

    centralLayout->addWidget(_tabWidget, 1);

    initShellUi();
    initSshUi();
    initSerialUi();
    initTelnetUi();

    // ── 底部按钮行（靠右）──────────────────────────────
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(12, 8, 12, 8);
    btnLayout->setSpacing(8);
    btnLayout->addStretch();

    auto* cancelBtn = new ElaPushButton(tr("Cancel"), _centralWidget);
    btnLayout->addWidget(cancelBtn);

    auto* confirmBtn = new ElaPushButton(tr("Confirm"), _centralWidget);
    btnLayout->addWidget(confirmBtn);

    centralLayout->addLayout(btnLayout);

    // ── 信号连接 ──
    connect(cancelBtn, &QPushButton::clicked,
            this, &SessionPage::dialogRejected);
    connect(confirmBtn, &QPushButton::clicked, this, [this]() {
        const auto type =
            (_shellTypeCombo->currentText() == QStringLiteral("PowerShell"))
                ? TerminalView::LocalShellType::PowerShell
                : TerminalView::LocalShellType::Cmd;
        emit localSessionRequested(type);
    });

    addCentralWidget(_centralWidget, true, true, 0);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void SessionPage::retranslateUi()
{

}


void SessionPage::initShellUi()
{
    // ── 右侧 Stacked 页面：本地 Shell 配置 ──────────────
    auto* page = new QWidget(_tabWidget);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    auto* typeLayout = new QHBoxLayout();
    typeLayout->setSpacing(8);

    // ── 本地 Shell：Shell 类型选择（cmd / PowerShell）──
    auto* localTypeLabel = new ElaText(tr("Type"), page);
    localTypeLabel->setWordWrap(false);
    localTypeLabel->setTextPixelSize(15);
    typeLayout->addWidget(localTypeLabel);

    _shellTypeCombo = new ElaComboBox(page);
    _shellTypeCombo->addItem(QStringLiteral("cmd"));
    _shellTypeCombo->addItem(QStringLiteral("PowerShell"));
    _shellTypeCombo->setMinimumWidth(160);
    typeLayout->addWidget(_shellTypeCombo);
    typeLayout->addStretch();

    pageLayout->addLayout(typeLayout);

    //标签行
    auto* labelLayout = new QHBoxLayout();
    labelLayout->setSpacing(8);
    auto* localLabel = new ElaText(tr("Label"), page);
    localLabel->setWordWrap(false);
    localLabel->setTextPixelSize(15);
    labelLayout->addWidget(localLabel);

    _shellLabel = new ElaLineEdit(page);
    _shellLabel->setText("sss");
    labelLayout->addWidget(_shellLabel);

    labelLayout->addStretch();

    pageLayout->addLayout(labelLayout);
    pageLayout->addStretch();

    _tabWidget->addTab(page, tr("local shell"));
}

void SessionPage::initSshUi()
{
    // ── 右侧 Stacked 页面：本地 ssh配置 ──────────────
    auto* page = new QWidget(_tabWidget);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    // ip地址
    auto* ipLayout = new QHBoxLayout();
    ipLayout->setSpacing(8);
    auto* ipLabel = new ElaText(tr("IP"), page);
    ipLabel->setWordWrap(false);
    ipLabel->setTextPixelSize(15);
    ipLayout->addWidget(ipLabel);

    _sshIp = new ElaLineEdit(page);
    _sshIp->setText("_ip");
    ipLayout->addWidget(_sshIp);

    ipLayout->addStretch();
    pageLayout->addLayout(ipLayout);

    // 用户名
    auto* nameLayout = new QHBoxLayout();
    nameLayout->setSpacing(8);
    auto* userNameLabel = new ElaText(tr("User Name"), page);
    userNameLabel->setWordWrap(false);
    userNameLabel->setTextPixelSize(15);
    nameLayout->addWidget(userNameLabel);

    _sshUserName = new ElaLineEdit(page);
    _sshUserName->setText("_userName");
    nameLayout->addWidget(_sshUserName);

    nameLayout->addStretch();
    pageLayout->addLayout(nameLayout);

    // 密码
    auto* passwordLayout = new QHBoxLayout();
    passwordLayout->setSpacing(8);
    auto* passwordLabel = new ElaText(tr("Password"), page);
    passwordLabel->setWordWrap(false);
    passwordLabel->setTextPixelSize(15);
    passwordLayout->addWidget(passwordLabel);

    _sshPassword = new ElaLineEdit(page);
    _sshPassword->setText("_password");
    passwordLayout->addWidget(_sshPassword);

    passwordLayout->addStretch();
    pageLayout->addLayout(passwordLayout);

    //标签行
    auto* labelLayout = new QHBoxLayout();
    labelLayout->setSpacing(8);
    auto* localLabel = new ElaText(tr("Label"), page);
    localLabel->setWordWrap(false);
    localLabel->setTextPixelSize(15);
    labelLayout->addWidget(localLabel);

    _sshLabel = new ElaLineEdit(page);
    _sshLabel->setText("sss");
    labelLayout->addWidget(_sshLabel);

    labelLayout->addStretch();

    pageLayout->addLayout(labelLayout);
    pageLayout->addStretch();

    // 占位页面（后续实现）
    _tabWidget->addTab(page, tr("ssh"));
}


void SessionPage::initSerialUi()
{
    // ── 右侧 Stacked 页面：本地 serial 配置 ──────────────
    auto* page = new QWidget(_tabWidget);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    auto* portLayout = new QHBoxLayout();
    portLayout->setSpacing(8);

    // 串口号
    auto* portLabel = new ElaText(tr("Port Num"), page);
    portLabel->setWordWrap(false);
    portLabel->setTextPixelSize(15);
    portLayout->addWidget(portLabel);

    _portCombo = new ElaComboBox(page);
    _portCombo->addItem("sss");
    _portCombo->addItem("sss");
    _portCombo->addItem("sss");
    portLayout->addWidget(_portCombo);

    portLayout->addStretch();
    pageLayout->addLayout(portLayout);

    // 波特率
    auto* baudRateLayout = new QHBoxLayout();
    baudRateLayout->setSpacing(8);

    auto* baudRateLabel = new ElaText(tr("Baud Rate"), page);
    baudRateLabel->setWordWrap(false);
    baudRateLabel->setTextPixelSize(15);
    baudRateLayout->addWidget(baudRateLabel);

    _baudRateCombo = new ElaComboBox(page);
    _baudRateCombo->addItem("sss");
    _baudRateCombo->addItem("sss");
    _baudRateCombo->addItem("sss");
    baudRateLayout->addWidget(_baudRateCombo);

    baudRateLayout->addStretch();
    pageLayout->addLayout(baudRateLayout);

    // 校验位
    auto* checkLayout = new QHBoxLayout();
    checkLayout->setSpacing(8);

    auto* checkLabel = new ElaText(tr("Check"), page);
    checkLabel->setWordWrap(false);
    checkLabel->setTextPixelSize(15);
    checkLayout->addWidget(checkLabel);

    _checkCombo = new ElaComboBox(page);
    _checkCombo->addItem("sss");
    _checkCombo->addItem("sss");
    _checkCombo->addItem("sss");
    checkLayout->addWidget(_checkCombo);

    checkLayout->addStretch();
    pageLayout->addLayout(checkLayout);

    //标签行
    auto* labelLayout = new QHBoxLayout();
    labelLayout->setSpacing(8);
    auto* localLabel = new ElaText(tr("Label"), page);
    localLabel->setWordWrap(false);
    localLabel->setTextPixelSize(15);
    labelLayout->addWidget(localLabel);

    _serialLabel = new ElaLineEdit(page);
    _serialLabel->setText("sss");
    labelLayout->addWidget(_serialLabel);

    labelLayout->addStretch();

    pageLayout->addLayout(labelLayout);
    pageLayout->addStretch();

    _tabWidget->addTab(page, tr("serial port"));
}


void SessionPage::initTelnetUi()
{
    // ── 右侧 Stacked 页面：本地 telnet 配置 ──────────────
    auto* page = new QWidget(_tabWidget);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    // IP地址
    auto* ipLayout = new QHBoxLayout();
    ipLayout->setSpacing(8);
    auto*ipLabel = new ElaText(tr("IP"), page);
    ipLabel->setWordWrap(false);
    ipLabel->setTextPixelSize(15);
    ipLayout->addWidget(ipLabel);

    _telnetIp = new ElaLineEdit(page);
    _telnetIp->setText("sss");
    ipLayout->addWidget(_telnetIp);

    ipLayout->addStretch();
    pageLayout->addLayout(ipLayout);

    // Label 行
    auto* labelLayout = new QHBoxLayout();
    labelLayout->setSpacing(8);
    auto* localLabel = new ElaText(tr("Label"), page);
    localLabel->setWordWrap(false);
    localLabel->setTextPixelSize(15);
    labelLayout->addWidget(localLabel);

    _telnetLabel = new ElaLineEdit(page);
    _telnetLabel->setText("sss");
    labelLayout->addWidget(_telnetLabel);

    labelLayout->addStretch();

    pageLayout->addLayout(labelLayout);
    pageLayout->addStretch();

    _tabWidget->addTab(page, tr("telnet"));
}

