/**
 * @file SftpPanel.h
 * @brief 可停靠的 SFTP 快捷传输面板。
 */
#pragma once

#include <QWidget>

class ElaIconButton;
class QLabel;
class QLineEdit;
class QTreeWidget;

class SftpPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SftpPanel(QWidget* parent = nullptr);

    /** 更新当前终端标签及其是否为已连接的 SSH 会话。 */
    void setSessionContext(const QString& sessionLabel, bool sshSessionActive);

private:
    void retranslateUi();
    void refreshAvailability();

    QLabel* _sessionLabel{nullptr};
    QLabel* _availabilityLabel{nullptr};
    QLineEdit* _pathEdit{nullptr};
    ElaIconButton* _parentDirectoryButton{nullptr};
    ElaIconButton* _refreshButton{nullptr};
    ElaIconButton* _uploadButton{nullptr};
    ElaIconButton* _downloadButton{nullptr};
    QTreeWidget* _fileTree{nullptr};
    QString _sessionName;
    bool _sshSessionActive{false};
};
