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

    auto* grid = new QGridLayout(page);
    grid->setContentsMargins(16, 16, 16, 16);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(1, 1);   // 输入控件列撑满

    // ── Shell 类型选择（cmd / PowerShell）──
    auto* localTypeLabel = new ElaText(tr("Type"), page);
    localTypeLabel->setWordWrap(false);
    localTypeLabel->setTextPixelSize(15);
    grid->addWidget(localTypeLabel, 0, 0, Qt::AlignVCenter);

    _shellTypeCombo = new ElaComboBox(page);
    _shellTypeCombo->addItem(QStringLiteral("cmd"));
    _shellTypeCombo->addItem(QStringLiteral("PowerShell"));
    _shellTypeCombo->setMinimumWidth(160);
    grid->addWidget(_shellTypeCombo, 0, 1);

    // 标签行
    auto* localLabel = new ElaText(tr("Label"), page);
    localLabel->setWordWrap(false);
    localLabel->setTextPixelSize(15);
    grid->addWidget(localLabel, 1, 0, Qt::AlignVCenter);

    _shellLabel = new ElaLineEdit(page);
    _shellLabel->setText("sss");
    grid->addWidget(_shellLabel, 1, 1);

    grid->setRowStretch(2, 1);      // 尾部留白

    _tabWidget->addTab(page, tr("local shell"));
}

void SessionPage::initSshUi()
{
    // ── 右侧 Stacked 页面：本地 ssh 配置 ──────────────
    auto* page = new QWidget(_tabWidget);

    auto* grid = new QGridLayout(page);
    grid->setContentsMargins(16, 16, 16, 16);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(1, 1);

    // IP
    auto* ipLabel = new ElaText(tr("IP"), page);
    ipLabel->setWordWrap(false);
    ipLabel->setTextPixelSize(15);
    grid->addWidget(ipLabel, 0, 0, Qt::AlignVCenter);

    _sshIp = new ElaLineEdit(page);
    _sshIp->setText("_ip");
    grid->addWidget(_sshIp, 0, 1);

    // 用户名
    auto* userNameLabel = new ElaText(tr("User Name"), page);
    userNameLabel->setWordWrap(false);
    userNameLabel->setTextPixelSize(15);
    grid->addWidget(userNameLabel, 1, 0, Qt::AlignVCenter);

    _sshUserName = new ElaLineEdit(page);
    _sshUserName->setText("_userName");
    grid->addWidget(_sshUserName, 1, 1);

    // 密码
    auto* passwordLabel = new ElaText(tr("Password"), page);
    passwordLabel->setWordWrap(false);
    passwordLabel->setTextPixelSize(15);
    grid->addWidget(passwordLabel, 2, 0, Qt::AlignVCenter);

    _sshPassword = new ElaLineEdit(page);
    _sshPassword->setText("_password");
    grid->addWidget(_sshPassword, 2, 1);

    // 标签
    auto* sshLabelHint = new ElaText(tr("Label"), page);
    sshLabelHint->setWordWrap(false);
    sshLabelHint->setTextPixelSize(15);
    grid->addWidget(sshLabelHint, 3, 0, Qt::AlignVCenter);

    _sshLabel = new ElaLineEdit(page);
    _sshLabel->setText("sss");
    grid->addWidget(_sshLabel, 3, 1);

    grid->setRowStretch(4, 1);

    _tabWidget->addTab(page, tr("ssh"));
}


void SessionPage::initSerialUi()
{
    // ── 右侧 Stacked 页面：本地 serial 配置 ──────────────
    auto* page = new QWidget(_tabWidget);

    auto* grid = new QGridLayout(page);
    grid->setContentsMargins(16, 16, 16, 16);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(1, 1);

    // 串口号
    auto* portLabel = new ElaText(tr("Port Num"), page);
    portLabel->setWordWrap(false);
    portLabel->setTextPixelSize(15);
    grid->addWidget(portLabel, 0, 0, Qt::AlignVCenter);

    _portCombo = new ElaComboBox(page);
    _portCombo->addItem("sss");
    _portCombo->addItem("sss");
    _portCombo->addItem("sss");
    grid->addWidget(_portCombo, 0, 1);

    // 波特率
    auto* baudRateLabel = new ElaText(tr("Baud Rate"), page);
    baudRateLabel->setWordWrap(false);
    baudRateLabel->setTextPixelSize(15);
    grid->addWidget(baudRateLabel, 1, 0, Qt::AlignVCenter);

    _baudRateCombo = new ElaComboBox(page);
    _baudRateCombo->addItem("sss");
    _baudRateCombo->addItem("sss");
    _baudRateCombo->addItem("sss");
    grid->addWidget(_baudRateCombo, 1, 1);

    // 校验位
    auto* checkLabel = new ElaText(tr("Check"), page);
    checkLabel->setWordWrap(false);
    checkLabel->setTextPixelSize(15);
    grid->addWidget(checkLabel, 2, 0, Qt::AlignVCenter);

    _checkCombo = new ElaComboBox(page);
    _checkCombo->addItem("sss");
    _checkCombo->addItem("sss");
    _checkCombo->addItem("sss");
    grid->addWidget(_checkCombo, 2, 1);

    // 标签
    auto* serialLabelHint = new ElaText(tr("Label"), page);
    serialLabelHint->setWordWrap(false);
    serialLabelHint->setTextPixelSize(15);
    grid->addWidget(serialLabelHint, 3, 0, Qt::AlignVCenter);

    _serialLabel = new ElaLineEdit(page);
    _serialLabel->setText("sss");
    grid->addWidget(_serialLabel, 3, 1);

    grid->setRowStretch(4, 1);

    _tabWidget->addTab(page, tr("serial port"));
}


void SessionPage::initTelnetUi()
{
    // ── 右侧 Stacked 页面：本地 telnet 配置 ──────────────
    auto* page = new QWidget(_tabWidget);

    auto* grid = new QGridLayout(page);
    grid->setContentsMargins(16, 16, 16, 16);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(12);
    grid->setColumnStretch(1, 1);

    // IP
    auto* ipLabel = new ElaText(tr("IP"), page);
    ipLabel->setWordWrap(false);
    ipLabel->setTextPixelSize(15);
    grid->addWidget(ipLabel, 0, 0, Qt::AlignVCenter);

    _telnetIp = new ElaLineEdit(page);
    _telnetIp->setText("sss");
    grid->addWidget(_telnetIp, 0, 1);

    // 标签
    auto* telnetLabelHint = new ElaText(tr("Label"), page);
    telnetLabelHint->setWordWrap(false);
    telnetLabelHint->setTextPixelSize(15);
    grid->addWidget(telnetLabelHint, 1, 0, Qt::AlignVCenter);

    _telnetLabel = new ElaLineEdit(page);
    _telnetLabel->setText("sss");
    grid->addWidget(_telnetLabel, 1, 1);

    grid->setRowStretch(2, 1);

    _tabWidget->addTab(page, tr("telnet"));
}

