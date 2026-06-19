#pragma once
#include "ElaScrollPage.h"

class ElaTabWidget;
class ElaText;
class ElaPushButton;

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
    void localSessionRequested();   // 用户在本地 Shell 标签页点击了 Confirm
    void dialogRejected();          // 用户在任意标签页点击了 Cancel

private:
    void retranslateUi();

    ElaTabWidget* _tabWidget{nullptr};
    QWidget* _centralWidget{nullptr};

    // ── 各标签页的占位文本 ──
    ElaText* _localPlaceholder{nullptr};
    ElaText* _sshPlaceholder{nullptr};
    ElaText* _serialPlaceholder{nullptr};
    ElaText* _telnetPlaceholder{nullptr};

    // ── 各标签页的按钮 ──
    ElaPushButton* _localConfirmBtn{nullptr};
    ElaPushButton* _localCancelBtn{nullptr};
    ElaPushButton* _sshConfirmBtn{nullptr};
    ElaPushButton* _sshCancelBtn{nullptr};
    ElaPushButton* _serialConfirmBtn{nullptr};
    ElaPushButton* _serialCancelBtn{nullptr};
    ElaPushButton* _telnetConfirmBtn{nullptr};
    ElaPushButton* _telnetCancelBtn{nullptr};
};
