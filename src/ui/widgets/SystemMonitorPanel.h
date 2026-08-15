/**
 * @file SystemMonitorPanel.h
 * @brief 可停靠的远端系统资源监视面板。
 */
#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;
class QTreeWidget;

class SystemMonitorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SystemMonitorPanel(QWidget* parent = nullptr);

    /** 更新当前终端标签及其是否为已连接的 SSH 会话。 */
    void setSessionContext(const QString& sessionLabel, bool sshSessionActive);

private:
    void retranslateUi();
    void refreshAvailability();

    QLabel* _sessionLabel{nullptr};
    QLabel* _availabilityLabel{nullptr};
    QLabel* _cpuLabel{nullptr};
    QLabel* _memoryLabel{nullptr};
    QLabel* _swapLabel{nullptr};
    QLabel* _networkLabel{nullptr};
    QLabel* _diskLabel{nullptr};
    QProgressBar* _cpuProgress{nullptr};
    QProgressBar* _memoryProgress{nullptr};
    QProgressBar* _swapProgress{nullptr};
    QTreeWidget* _networkTree{nullptr};
    QTreeWidget* _diskTree{nullptr};
    QString _sessionName;
    bool _sshSessionActive{false};
};
