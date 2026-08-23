/**
 * @file   SessionPage.cpp
 * @brief  会话选择页面实现：多协议标签页 UI 构建与配置收集。
 *
 * 按 4 个标签页（本地 Shell / SSH / 串口 / Telnet）构建控件，Confirm 时
 * 从控件收集配置并通过信号上报。applyRuntimeConfig() 用于编辑已有会话时回填。
 */
#include "SessionPage.h"
#include "ElaComboBox.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "service/LanguageManager.h"
#include "transport/serialport_info.h"
#include "ElaTabWidget.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSerialPort>
#include <QSignalBlocker>
#include <QMessageBox>
#include <QVBoxLayout>

namespace {

// QComboBox 默认 UserRole 保存 Shell 类型，额外角色保存 WSL 发行版名称。
constexpr int WslDistributionRole = Qt::UserRole + 1;

void setIpv4Validator(QLineEdit* lineEdit)
{
    static const QRegularExpression ipv4Pattern(QStringLiteral(
        R"(^(?:(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])\.){3}(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])$)"));

    lineEdit->setMaxLength(15);
    lineEdit->setValidator(new QRegularExpressionValidator(ipv4Pattern, lineEdit));
}

ElaText* addFormLabel(QGridLayout* grid, int row, const QString& text,
                      QWidget* parent)
{
    auto* label = new ElaText(text, parent);
    label->setWordWrap(false);
    label->setTextPixelSize(15);
    grid->addWidget(label, row, 0, Qt::AlignVCenter);
    return label;
}

void configurePortSpinBox(ElaSpinBox* spinBox, int defaultPort)
{
    spinBox->setRange(1, 65535);
    spinBox->setValue(defaultPort);
    spinBox->setAlignment(Qt::AlignLeft);
}

void populateTerminalTypes(ElaComboBox* comboBox)
{
    comboBox->setEditable(true);
    comboBox->addItems({QStringLiteral("xterm-256color"),
                        QStringLiteral("xterm"),
                        QStringLiteral("vt100")});
}

} // namespace

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
        if (_tabWidget->currentIndex() == 2) {
            SerialConfig config;
            config.portName = _portCombo->currentText()
                                  .section(QLatin1Char(':'), 0, 0).trimmed();
            config.baudRate = _baudRateCombo->currentText().toInt();
            config.parity = static_cast<QSerialPort::Parity>(
                _parityCombo->currentData().toInt());
            config.dataBits = static_cast<QSerialPort::DataBits>(
                _dataBitsCombo->currentData().toInt());
            config.stopBits = static_cast<QSerialPort::StopBits>(
                _stopBitsCombo->currentData().toInt());
            config.flowControl = static_cast<QSerialPort::FlowControl>(
                _flowControlCombo->currentData().toInt());
            config.label = _serialLabel->text().trimmed();

            if (!config.isValid()) {
                QMessageBox::warning(this, tr("Serial Session"),
                                     tr("Select a serial port and provide a valid baud rate."));
                return;
            }
            emit serialSessionRequested(config);
            return;
        }

        if (_tabWidget->currentIndex() == 1) {
            // ── SSH 标签页（local shell=0, ssh=1, serial=2, telnet=3）──
            SshConfig config;
            config.host = _sshIp->text().trimmed();
            config.port = static_cast<quint16>(_sshPort->value());
            config.username = _sshUserName->text().trimmed();
            config.authMethod = _sshAuthMethod->currentData().toString();
            config.password = _sshPassword->text();
            config.privateKeyPath = _sshPrivateKey->text().trimmed();
            config.keyPassphrase = _sshKeyPassphrase->text();
            config.terminalType = _sshTerminalType->currentText().trimmed();
            config.keepAliveSeconds = _sshKeepAlive->value();
            config.label = _sshLabel->text().trimmed();

            if (!config.isValid()) {
                QMessageBox::warning(
                    this, tr("SSH Session"),
                    tr("Provide a host, user name and the credentials for the "
                       "selected authentication method."));
                return;
            }
            emit sshSessionRequested(config);
            return;
        }

        const auto type = static_cast<TerminalView::LocalShellType>(
            _shellTypeCombo->currentData().toInt());
        const QString wslDistribution = type == TerminalView::LocalShellType::Wsl
            ? _shellTypeCombo->currentData(WslDistributionRole).toString()
            : QString{};
        emit localSessionRequested(type, wslDistribution,
                                   _shellLabel->text().trimmed());
    });

    addCentralWidget(_centralWidget, true, true, 0);

    // 动态语言切换
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
}

void SessionPage::retranslateUi()
{

}

void SessionPage::selectTransport(TransportKind kind)
{
    int index = 0;
    switch (kind) {
    case TransportKind::Ssh:
        index = 1;
        break;
    case TransportKind::Serial:
        index = 2;
        break;
    case TransportKind::Telnet:
        index = 3;
        break;
    case TransportKind::LocalShell:
    case TransportKind::Custom:
        break;
    }
    _tabWidget->setCurrentIndex(index);
}

void SessionPage::applyRuntimeConfig(const RuntimeConfig& runtime,
                                     const QByteArray& secret)
{
    selectTransport(runtime.transportKind);
    const QVariantMap& values = runtime.transport;

    switch (runtime.transportKind) {
    case TransportKind::LocalShell: {
        const auto shellType = static_cast<TerminalView::LocalShellType>(
            values.value(QStringLiteral("shellType")).toInt());
        const QString wslDistribution = values.value(
            QStringLiteral("wslDistribution")).toString();
        int shellIndex = -1;
        for (int index = 0; index < _shellTypeCombo->count(); ++index) {
            const auto itemType = static_cast<TerminalView::LocalShellType>(
                _shellTypeCombo->itemData(index).toInt());
            const bool sameDistribution = itemType != TerminalView::LocalShellType::Wsl
                || _shellTypeCombo->itemData(index, WslDistributionRole).toString()
                    == wslDistribution;
            if (itemType == shellType && sameDistribution) {
                shellIndex = index;
                break;
            }
        }
        if (shellIndex < 0 && shellType == TerminalView::LocalShellType::Wsl
            && !wslDistribution.isEmpty()) {
            // 编辑历史会话时，即使对应发行版当前已被移除，也保留原配置供用户识别。
            _shellTypeCombo->addItem(
                QStringLiteral("WSL (%1)").arg(wslDistribution),
                static_cast<int>(TerminalView::LocalShellType::Wsl));
            shellIndex = _shellTypeCombo->count() - 1;
            _shellTypeCombo->setItemData(shellIndex, wslDistribution,
                                         WslDistributionRole);
        }
        _shellTypeCombo->setCurrentIndex(shellIndex >= 0 ? shellIndex : 0);
        _shellLabel->setText(values.value(QStringLiteral("label")).toString());
        break;
    }
    case TransportKind::Ssh: {
        _sshIp->setText(values.value(QStringLiteral("host")).toString());
        _sshPort->setValue(values.value(QStringLiteral("port"), 22).toInt());
        _sshUserName->setText(
            values.value(QStringLiteral("username")).toString());
        const int authIndex = _sshAuthMethod->findData(
            values.value(QStringLiteral("authMethod"),
                         QStringLiteral("password")));
        _sshAuthMethod->setCurrentIndex(authIndex >= 0 ? authIndex : 0);
        _sshPassword->clear();
        _sshKeyPassphrase->clear();
        if (_sshAuthMethod->currentData().toString()
            == QStringLiteral("password")) {
            _sshPassword->setText(QString::fromUtf8(secret));
        } else {
            _sshKeyPassphrase->setText(QString::fromUtf8(secret));
        }
        _sshPrivateKey->setText(
            values.value(QStringLiteral("privateKeyPath")).toString());
        _sshTerminalType->setCurrentText(
            values.value(QStringLiteral("terminalType"),
                         QStringLiteral("xterm-256color")).toString());
        _sshKeepAlive->setValue(
            values.value(QStringLiteral("keepAliveSeconds"), 30).toInt());
        _sshLabel->setText(values.value(QStringLiteral("label")).toString());
        break;
    }
    case TransportKind::Serial: {
        const QString portName =
            values.value(QStringLiteral("portName")).toString();
        int portIndex = -1;
        for (int index = 0; index < _portCombo->count(); ++index) {
            if (_portCombo->itemText(index)
                    .section(QLatin1Char(':'), 0, 0).trimmed() == portName) {
                portIndex = index;
                break;
            }
        }
        if (portIndex < 0 && !portName.isEmpty()) {
            _portCombo->addItem(portName);
            portIndex = _portCombo->count() - 1;
        }
        _portCombo->setCurrentIndex(portIndex);
        _baudRateCombo->setCurrentText(
            QString::number(values.value(QStringLiteral("baudRate"),
                                         115200).toInt()));
        const auto setCurrentData = [](QComboBox* combo, int value) {
            const int index = combo->findData(value);
            if (index >= 0)
                combo->setCurrentIndex(index);
        };
        setCurrentData(_dataBitsCombo,
                       values.value(QStringLiteral("dataBits"),
                                    QSerialPort::Data8).toInt());
        setCurrentData(_parityCombo,
                       values.value(QStringLiteral("parity"),
                                    QSerialPort::NoParity).toInt());
        setCurrentData(_stopBitsCombo,
                       values.value(QStringLiteral("stopBits"),
                                    QSerialPort::OneStop).toInt());
        setCurrentData(_flowControlCombo,
                       values.value(QStringLiteral("flowControl"),
                                    QSerialPort::NoFlowControl).toInt());
        _serialLabel->setText(
            values.value(QStringLiteral("label")).toString());
        break;
    }
    case TransportKind::Telnet:
        _telnetIp->setText(values.value(QStringLiteral("host")).toString());
        _telnetPort->setValue(values.value(QStringLiteral("port"), 23).toInt());
        _telnetTerminalType->setCurrentText(
            values.value(QStringLiteral("terminalType"),
                         QStringLiteral("xterm-256color")).toString());
        _telnetNaws->setChecked(
            values.value(QStringLiteral("naws"), true).toBool());
        _telnetBinaryMode->setChecked(
            values.value(QStringLiteral("binaryMode"), false).toBool());
        _telnetKeepAlive->setValue(
            values.value(QStringLiteral("keepAliveSeconds"), 0).toInt());
        _telnetLabel->setText(
            values.value(QStringLiteral("label")).toString());
        break;
    case TransportKind::Custom:
        break;
    }
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

    // ── Shell 类型选择 ─────────────────────────────────
    auto* localTypeLabel = new ElaText(tr("Type"), page);
    localTypeLabel->setWordWrap(false);
    localTypeLabel->setTextPixelSize(15);
    grid->addWidget(localTypeLabel, 0, 0, Qt::AlignVCenter);

    _shellTypeCombo = new ElaComboBox(page);
#ifdef Q_OS_WIN
    // Windows：cmd / PowerShell / 已安装的 WSL 发行版。
    _shellTypeCombo->addItem(
        QStringLiteral("cmd"),
        static_cast<int>(TerminalView::LocalShellType::Cmd));
    _shellTypeCombo->addItem(
        QStringLiteral("PowerShell"),
        static_cast<int>(TerminalView::LocalShellType::PowerShell));
    // 仅把 wsl.exe 实际返回的发行版加入列表；功能未启用或尚未安装实例时
    // 不提供一个必然启动失败的通用 WSL 选项。
    const auto wslResult = LocalShellProfiles::discoverWslDistributions();
    for (const QString& distribution : wslResult.distributions) {
        _shellTypeCombo->addItem(
            QStringLiteral("WSL (%1)").arg(distribution),
            static_cast<int>(TerminalView::LocalShellType::Wsl));
        _shellTypeCombo->setItemData(_shellTypeCombo->count() - 1,
                                     distribution, WslDistributionRole);
    }
    if (wslResult.status == LocalShellProfiles::WslDiscoveryStatus::Unavailable) {
        _shellTypeCombo->setToolTip(
            tr("WSL is unavailable on this Windows system."));
    } else if (wslResult.status
               == LocalShellProfiles::WslDiscoveryStatus::NoDistributions) {
        _shellTypeCombo->setToolTip(
            tr("WSL is enabled, but no distribution is installed."));
    }
#else
    // Unix：始终启动登录 shell（$SHELL，回退 /bin/bash）。LocalShellType 在
    // Unix 下被 TerminalView::startLocalShell() 忽略，此处仅展示实际将启动
    // 的 shell 路径，类型固定为 Cmd（枚举占位，保持 Confirm 逻辑一致）。
    const auto defaultShell = LocalShellProfiles::platformDefault();
    _shellTypeCombo->addItem(
        QStringLiteral("%1 (%2)").arg(defaultShell.name,
                                      defaultShell.executable),
        static_cast<int>(TerminalView::LocalShellType::Cmd));
#endif
    _shellTypeCombo->setMinimumWidth(160);
    grid->addWidget(_shellTypeCombo, 0, 1);

    // 标签行
    auto* localLabel = new ElaText(tr("Label"), page);
    localLabel->setWordWrap(false);
    localLabel->setTextPixelSize(15);
    grid->addWidget(localLabel, 1, 0, Qt::AlignVCenter);

    _shellLabel = new ElaLineEdit(page);
    _shellLabel->setPlaceholderText(tr("Optional session name"));
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

    // 连接
    addFormLabel(grid, 0, tr("IPv4 Address"), page);
    _sshIp = new ElaLineEdit(page);
    _sshIp->setPlaceholderText(tr("IPv4 address, e.g. 192.168.0.1"));
    _sshIp->setClearButtonEnabled(true);
    setIpv4Validator(_sshIp);
    grid->addWidget(_sshIp, 0, 1);

    addFormLabel(grid, 1, tr("Port"), page);
    _sshPort = new ElaSpinBox(page);
    configurePortSpinBox(_sshPort, 22);
    grid->addWidget(_sshPort, 1, 1);

    // 认证
    addFormLabel(grid, 2, tr("User Name"), page);
    _sshUserName = new ElaLineEdit(page);
    _sshUserName->setPlaceholderText(tr("User name"));
    _sshUserName->setClearButtonEnabled(true);
    grid->addWidget(_sshUserName, 2, 1);

    addFormLabel(grid, 3, tr("Authentication"), page);
    _sshAuthMethod = new ElaComboBox(page);
    _sshAuthMethod->addItem(tr("Password"), QStringLiteral("password"));
    _sshAuthMethod->addItem(tr("Private Key"), QStringLiteral("publickey"));
    grid->addWidget(_sshAuthMethod, 3, 1);

    addFormLabel(grid, 4, tr("Password"), page);
    _sshPassword = new ElaLineEdit(page);
    _sshPassword->setPlaceholderText(tr("Password"));
    _sshPassword->setEchoMode(QLineEdit::Password);
    grid->addWidget(_sshPassword, 4, 1);

    addFormLabel(grid, 5, tr("Private Key"), page);
    auto* privateKeyLayout = new QHBoxLayout();
    privateKeyLayout->setContentsMargins(0, 0, 0, 0);
    privateKeyLayout->setSpacing(8);
    _sshPrivateKey = new ElaLineEdit(page);
    _sshPrivateKey->setPlaceholderText(tr("Private key file"));
    _sshPrivateKey->setClearButtonEnabled(true);
    auto* browseKeyButton = new ElaPushButton(tr("Browse..."), page);
    privateKeyLayout->addWidget(_sshPrivateKey, 1);
    privateKeyLayout->addWidget(browseKeyButton);
    grid->addLayout(privateKeyLayout, 5, 1);

    addFormLabel(grid, 6, tr("Key Passphrase"), page);
    _sshKeyPassphrase = new ElaLineEdit(page);
    _sshKeyPassphrase->setPlaceholderText(tr("Optional passphrase"));
    _sshKeyPassphrase->setEchoMode(QLineEdit::Password);
    grid->addWidget(_sshKeyPassphrase, 6, 1);

    // 终端行为
    addFormLabel(grid, 7, tr("Terminal Type"), page);
    _sshTerminalType = new ElaComboBox(page);
    populateTerminalTypes(_sshTerminalType);
    grid->addWidget(_sshTerminalType, 7, 1);

    addFormLabel(grid, 8, tr("Keep Alive"), page);
    _sshKeepAlive = new ElaSpinBox(page);
    _sshKeepAlive->setRange(0, 3600);
    _sshKeepAlive->setValue(30);
    _sshKeepAlive->setSuffix(tr(" s"));
    _sshKeepAlive->setSpecialValueText(tr("Disabled"));
    grid->addWidget(_sshKeepAlive, 8, 1);

    addFormLabel(grid, 9, tr("Label"), page);
    _sshLabel = new ElaLineEdit(page);
    _sshLabel->setPlaceholderText(tr("Optional session name"));
    grid->addWidget(_sshLabel, 9, 1);

    const auto updateAuthenticationFields = [this, browseKeyButton](int index) {
        const bool usePrivateKey = index == 1;
        _sshPassword->setEnabled(!usePrivateKey);
        _sshPrivateKey->setEnabled(usePrivateKey);
        _sshKeyPassphrase->setEnabled(usePrivateKey);
        browseKeyButton->setEnabled(usePrivateKey);
    };
    connect(_sshAuthMethod, qOverload<int>(&QComboBox::currentIndexChanged),
            this, updateAuthenticationFields);
    connect(browseKeyButton, &QPushButton::clicked, this, [this, page]() {
        const QString path = QFileDialog::getOpenFileName(
            page, tr("Select SSH Private Key"), QString(),
            tr("Private keys (*)"));
        if (!path.isEmpty())
            _sshPrivateKey->setText(path);
    });
    updateAuthenticationFields(_sshAuthMethod->currentIndex());

    grid->setRowStretch(10, 1);

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

    // 串口设备与帧格式
    addFormLabel(grid, 0, tr("Port"), page);
    _portCombo = new ElaComboBox(page);
    grid->addWidget(_portCombo, 0, 1);

    const auto refreshSerialPorts = [this](const QStringList& ports,
                                           bool preserveSelection) {
        const QString selectedPort =
            _portCombo->currentText().section(QLatin1Char(':'), 0, 0);
        const QSignalBlocker blocker(_portCombo);

        _portCombo->clear();
        _portCombo->addItems(ports);
        _portCombo->setPlaceholderText(
            ports.isEmpty() ? tr("No serial ports detected")
                            : tr("Select a serial port"));

        if (!preserveSelection) {
            _portCombo->setCurrentIndex(ports.isEmpty() ? -1 : 0);
            return;
        }

        int selectedIndex = -1;
        for (int index = 0; index < ports.size(); ++index) {
            const QString port = ports.at(index).section(QLatin1Char(':'), 0, 0);
            if (port == selectedPort) {
                selectedIndex = index;
                break;
            }
        }
        if (selectedIndex < 0 && !selectedPort.trimmed().isEmpty()) {
            _portCombo->addItem(selectedPort.trimmed());
            selectedIndex = _portCombo->count() - 1;
        }
        _portCombo->setCurrentIndex(selectedIndex);
    };

    auto* serialPortInfo = new SerialPortInfo(page);
    connect(serialPortInfo, &SerialPortInfo::update, this,
            [refreshSerialPorts](const QStringList& ports) {
                refreshSerialPorts(ports, true);
            });
    refreshSerialPorts(SerialPortInfo::availablePorts(), false);

    addFormLabel(grid, 1, tr("Baud Rate"), page);
    _baudRateCombo = new ElaComboBox(page);
    _baudRateCombo->setEditable(true);
    _baudRateCombo->setInsertPolicy(QComboBox::NoInsert);
    _baudRateCombo->addItems({QStringLiteral("110"), QStringLiteral("300"),
                              QStringLiteral("600"), QStringLiteral("1200"),
                              QStringLiteral("2400"), QStringLiteral("4800"),
                              QStringLiteral("9600"), QStringLiteral("19200"),
                              QStringLiteral("38400"), QStringLiteral("57600"),
                              QStringLiteral("115200"), QStringLiteral("230400"),
                              QStringLiteral("460800"), QStringLiteral("921600")});
    _baudRateCombo->lineEdit()->setValidator(
        new QIntValidator(1, 4000000, _baudRateCombo));
    _baudRateCombo->setCurrentText(QStringLiteral("115200"));
    grid->addWidget(_baudRateCombo, 1, 1);

    addFormLabel(grid, 2, tr("Parity"), page);
    _parityCombo = new ElaComboBox(page);
    _parityCombo->addItem(tr("None"), QSerialPort::NoParity);
    _parityCombo->addItem(tr("Even"), QSerialPort::EvenParity);
    _parityCombo->addItem(tr("Odd"), QSerialPort::OddParity);
    _parityCombo->addItem(tr("Mark"), QSerialPort::MarkParity);
    _parityCombo->addItem(tr("Space"), QSerialPort::SpaceParity);
    grid->addWidget(_parityCombo, 2, 1);

    addFormLabel(grid, 3, tr("Data Bits"), page);
    _dataBitsCombo = new ElaComboBox(page);
    _dataBitsCombo->addItem(QStringLiteral("5"), QSerialPort::Data5);
    _dataBitsCombo->addItem(QStringLiteral("6"), QSerialPort::Data6);
    _dataBitsCombo->addItem(QStringLiteral("7"), QSerialPort::Data7);
    _dataBitsCombo->addItem(QStringLiteral("8"), QSerialPort::Data8);
    _dataBitsCombo->setCurrentIndex(3);
    grid->addWidget(_dataBitsCombo, 3, 1);

    addFormLabel(grid, 4, tr("Stop Bits"), page);
    _stopBitsCombo = new ElaComboBox(page);
    _stopBitsCombo->addItem(QStringLiteral("1"), QSerialPort::OneStop);
    _stopBitsCombo->addItem(QStringLiteral("1.5"), QSerialPort::OneAndHalfStop);
    _stopBitsCombo->addItem(QStringLiteral("2"), QSerialPort::TwoStop);
    grid->addWidget(_stopBitsCombo, 4, 1);

    addFormLabel(grid, 5, tr("Flow Control"), page);
    _flowControlCombo = new ElaComboBox(page);
    _flowControlCombo->addItem(tr("None"), QSerialPort::NoFlowControl);
    _flowControlCombo->addItem(tr("Hardware (RTS/CTS)"),
                               QSerialPort::HardwareControl);
    _flowControlCombo->addItem(tr("Software (XON/XOFF)"),
                               QSerialPort::SoftwareControl);
    grid->addWidget(_flowControlCombo, 5, 1);

    addFormLabel(grid, 6, tr("Label"), page);
    _serialLabel = new ElaLineEdit(page);
    _serialLabel->setPlaceholderText(tr("Optional session name"));
    grid->addWidget(_serialLabel, 6, 1);

    grid->setRowStretch(7, 1);

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

    auto* securityWarning = new ElaText(
        tr("Warning: Telnet sends all data without encryption."), page);
    securityWarning->setWordWrap(true);
    securityWarning->setTextPixelSize(14);
    grid->addWidget(securityWarning, 0, 0, 1, 2);

    addFormLabel(grid, 1, tr("IPv4 Address"), page);
    _telnetIp = new ElaLineEdit(page);
    _telnetIp->setPlaceholderText(tr("IPv4 address, e.g. 192.168.0.1"));
    _telnetIp->setClearButtonEnabled(true);
    setIpv4Validator(_telnetIp);
    grid->addWidget(_telnetIp, 1, 1);

    addFormLabel(grid, 2, tr("Port"), page);
    _telnetPort = new ElaSpinBox(page);
    configurePortSpinBox(_telnetPort, 23);
    grid->addWidget(_telnetPort, 2, 1);

    addFormLabel(grid, 3, tr("Terminal Type"), page);
    _telnetTerminalType = new ElaComboBox(page);
    populateTerminalTypes(_telnetTerminalType);
    grid->addWidget(_telnetTerminalType, 3, 1);

    addFormLabel(grid, 4, tr("Negotiation"), page);
    auto* negotiationLayout = new QHBoxLayout();
    negotiationLayout->setContentsMargins(0, 0, 0, 0);
    negotiationLayout->setSpacing(16);
    _telnetNaws = new ElaCheckBox(tr("Window size (NAWS)"), page);
    _telnetNaws->setChecked(true);
    _telnetBinaryMode = new ElaCheckBox(tr("Binary mode"), page);
    negotiationLayout->addWidget(_telnetNaws);
    negotiationLayout->addWidget(_telnetBinaryMode);
    negotiationLayout->addStretch();
    grid->addLayout(negotiationLayout, 4, 1);

    addFormLabel(grid, 5, tr("Keep Alive"), page);
    _telnetKeepAlive = new ElaSpinBox(page);
    _telnetKeepAlive->setRange(0, 3600);
    _telnetKeepAlive->setValue(0);
    _telnetKeepAlive->setSuffix(tr(" s"));
    _telnetKeepAlive->setSpecialValueText(tr("Disabled"));
    grid->addWidget(_telnetKeepAlive, 5, 1);

    addFormLabel(grid, 6, tr("Label"), page);
    _telnetLabel = new ElaLineEdit(page);
    _telnetLabel->setPlaceholderText(tr("Optional session name"));
    grid->addWidget(_telnetLabel, 6, 1);

    grid->setRowStretch(7, 1);

    _tabWidget->addTab(page, tr("telnet"));
}
