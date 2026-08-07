#pragma once

#include "session/SessionTypes.h"

#include <QDialog>

// 主机密钥验证对话框：首次连接或主机密钥变更时向用户展示服务器公钥指纹，
// 由用户决定信任并保存（Accept）或拒绝（Reject）。exec() 返回
// QDialog::Accepted 表示用户选择信任，调用方据此调用
// SshTransport::acceptHostKey() / rejectHostKey()。
class SshHostKeyDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SshHostKeyDialog(const SshHostKeyInfo& info,
                              QWidget* parent = nullptr);
};
