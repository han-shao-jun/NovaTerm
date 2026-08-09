#pragma once

#include "ElaScrollPage.h"
#include "ui/terminal/TerminalView.h"   // TerminalView::LocalShellType（信号参数）
#include "session/SessionTypes.h"
#include "ui/widgets/VerticalTabWidget.h"
#include <ElaComboBox.h>
#include <ElaCheckBox.h>
#include <ElaPushButton.h>
#include <ElaSpinBox.h>
#include <ElaTabWidget.h>
#include <ElaLineEdit.h>
#include <QByteArray>


// 会话选择页面（ElaScrollPage）。包含 4 个标签页（本地 Shell / SSH / 串口 / Telnet），
// 页面底部提供统一的 Confirm / Cancel 按钮。嵌入 ElaDialog 中使用，与
// SettingsPage 模式一致。各协议控件将作为后续 SessionConfig 的输入来源。
class SessionPage : public ElaScrollPage
{
    Q_OBJECT
public:
    Q_INVOKABLE explicit SessionPage(QWidget* parent = nullptr);
    void selectTransport(TransportKind kind);
    void applyRuntimeConfig(const RuntimeConfig& runtime,
                            const QByteArray& secret = {});

signals:
    // 用户在本地 Shell 标签页点击了 Confirm，携带所选的 Shell 类型
    // （cmd 关联 Clink / PowerShell）。
    void localSessionRequested(TerminalView::LocalShellType type,
                               const QString& label);
    void serialSessionRequested(const SerialConfig& config);
    void sshSessionRequested(const SshConfig& config);
    void dialogRejected();          // 用户在任意标签页点击了 Cancel

private:
    void retranslateUi();
    void initShellUi();
    void initSshUi();
    void initSerialUi();
    void initTelnetUi();

    QWidget* _centralWidget{nullptr};
    VerticalTabWidget* _tabWidget{nullptr};

    // ── 本地 Shell：Shell 类型选择（cmd / PowerShell）──
    ElaComboBox*_shellTypeCombo{nullptr};

    ElaLineEdit* _shellLabel{nullptr};

    ElaLineEdit* _sshIp{nullptr};
    ElaSpinBox* _sshPort{nullptr};
    ElaLineEdit* _sshUserName{nullptr};
    ElaComboBox* _sshAuthMethod{nullptr};
    ElaLineEdit* _sshPassword{nullptr};
    ElaLineEdit* _sshPrivateKey{nullptr};
    ElaLineEdit* _sshKeyPassphrase{nullptr};
    ElaComboBox* _sshTerminalType{nullptr};
    ElaSpinBox* _sshKeepAlive{nullptr};
    ElaLineEdit* _sshLabel{nullptr};

    ElaComboBox* _portCombo{nullptr};
    ElaComboBox* _baudRateCombo{nullptr};
    ElaComboBox* _parityCombo{nullptr};
    ElaComboBox* _dataBitsCombo{nullptr};
    ElaComboBox* _stopBitsCombo{nullptr};
    ElaComboBox* _flowControlCombo{nullptr};
    ElaLineEdit* _serialLabel{nullptr};


    ElaLineEdit* _telnetIp{nullptr};
    ElaSpinBox* _telnetPort{nullptr};
    ElaComboBox* _telnetTerminalType{nullptr};
    ElaCheckBox* _telnetNaws{nullptr};
    ElaCheckBox* _telnetBinaryMode{nullptr};
    ElaSpinBox* _telnetKeepAlive{nullptr};
    ElaLineEdit* _telnetLabel{nullptr};

};
