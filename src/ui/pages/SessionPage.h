#pragma once

#include "ElaScrollPage.h"
#include "ui/terminal/TerminalView.h"   // TerminalView::LocalShellType（信号参数）
#include "ui/widgets/VerticalTabWidget.h"
#include <ElaComboBox.h>
#include <ElaPushButton.h>
#include <ElaTabWidget.h>
#include <ElaLineEdit.h>


// 会话选择页面（ElaScrollPage）。包含 4 个标签页（本地 Shell / SSH / 串口 / Telnet），
// 每个标签页各有 Confirm / Cancel 按钮。嵌入 ElaDialog 中使用，与 SettingsPage
// 模式一致。本地 Shell 的 Confirm 通过 localSessionRequested() 信号通知 MainWindow
// 导航到终端页面；其他协议显示 "Not implemented" 消息。
class SessionPage : public ElaScrollPage
{
    Q_OBJECT
public:
    Q_INVOKABLE explicit SessionPage(QWidget* parent = nullptr);

signals:
    // 用户在本地 Shell 标签页点击了 Confirm，携带所选的 Shell 类型
    // （cmd 关联 Clink / PowerShell）。
    void localSessionRequested(TerminalView::LocalShellType type);
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
    ElaLineEdit* _sshUserName{nullptr};
    ElaLineEdit* _sshPassword{nullptr};
    ElaLineEdit* _sshLabel{nullptr};

    ElaComboBox* _portCombo{nullptr};
    ElaComboBox* _baudRateCombo{nullptr};
    ElaComboBox* _checkCombo{nullptr};
    ElaLineEdit* _serialLabel{nullptr};


    ElaLineEdit* _telnetIp{nullptr};
    ElaLineEdit* _telnetLabel{nullptr};

};
