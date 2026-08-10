/**
 * @file   SessionPage.h
 * @brief  会话选择页面（ElaScrollPage）。
 *
 * 包含 4 个标签页（本地 Shell / SSH / 串口 / Telnet），页面底部提供统一的
 * Confirm / Cancel 按钮。嵌入 ElaDialog 中使用，与 SettingsPage 模式一致。
 * 各协议控件将作为后续 SessionConfig 的输入来源。
 */
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

    /**
     * @brief 切换到指定传输类型的标签页。
     * @param kind 传输类型。
     */
    void selectTransport(TransportKind kind);

    /**
     * @brief 从运行时配置回填控件（用于编辑已有会话）。
     * @param runtime 运行时配置。
     * @param secret  凭据字节（SSH 密码/口令，可空）。
     */
    void applyRuntimeConfig(const RuntimeConfig& runtime,
                            const QByteArray& secret = {});

signals:
    /**
     * @brief 用户在本地 Shell 标签页点击了 Confirm。
     * @param type  Shell 类型（cmd 关联 Clink / PowerShell）。
     * @param label 显示标签。
     */
    void localSessionRequested(TerminalView::LocalShellType type,
                               const QString& label);
    void serialSessionRequested(const SerialConfig& config); ///< 串口会话确认
    void sshSessionRequested(const SshConfig& config);        ///< SSH 会话确认
    void dialogRejected();          ///< 用户在任意标签页点击了 Cancel

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
