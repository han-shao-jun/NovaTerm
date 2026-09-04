/**
 * @file SystemMonitorPanel.h
 * @brief 可停靠的卡片式远端系统资源监视面板。
 */
#pragma once

#include "ElaDef.h"

#include <QHash>
#include <QPair>
#include <QPointer>
#include <QVector>
#include <QWidget>

class ElaTreeWidget;
class ElaText;
class QLabel;
class QComboBox;
class QPaintEvent;
class QProgressBar;
class QPushButton;
class QTimer;
class SshTransport;
class TrafficChart;

class SystemMonitorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SystemMonitorPanel(QWidget* parent = nullptr);

    /** 更新当前终端标签及其已连接的 SSH transport。 */
    void setSessionContext(const QString& sessionLabel, SshTransport* transport);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void retranslateUi();
    void applyTheme();
    /**
     * @brief 显示或清除磁盘列表的空状态提示。
     * @param text 提示文案；传空字符串则隐藏提示。
     * @note  与 SftpPanel 同理，用 ElaText 而非占位 QTreeWidgetItem —— Ela 的树
     *        样式会把禁用态 item 文字画成 BasicTextDisable，占位文案会几乎看不见。
     */
    void setDiskTreeHint(const QString& text);
    void refreshAvailability();    /** 提交一次有界、非重入的远端资源采集请求。 */
    void requestMetrics();
    /** 校验请求归属，解析结果并用相邻样本计算 CPU/网络速率。 */
    void handleCommandFinished(quint64 requestId,
                               const QByteArray& standardOutput,
                               const QByteArray& standardError,
                               const QString& errorMessage);
    void updateNetworkView();
    /** 切换或断开会话时清除所有累计值基线。 */
    void resetMetrics();

    ElaText* _sessionLabel{nullptr};
    ElaText* _availabilityLabel{nullptr};
    QPushButton* _infoButton{nullptr};
    ElaText* _cpuLabel{nullptr};
    ElaText* _memoryLabel{nullptr};
    ElaText* _swapLabel{nullptr};
    QProgressBar* _cpuProgress{nullptr};
    QProgressBar* _memoryProgress{nullptr};
    QProgressBar* _swapProgress{nullptr};
    ElaText* _cpuDetail{nullptr};
    ElaText* _memoryDetail{nullptr};
    ElaText* _swapDetail{nullptr};
    QLabel* _receiveLabel{nullptr};
    QLabel* _sendLabel{nullptr};
    QComboBox* _interfaceCombo{nullptr};
    TrafficChart* _trafficChart{nullptr};
    ElaText* _pathHeader{nullptr};
    ElaText* _capacityHeader{nullptr};
    ElaTreeWidget* _diskTree{nullptr};
    ElaText* _diskTreeHint{nullptr};
    QTimer* _refreshTimer{nullptr};
    QPointer<SshTransport> _sshTransport;
    QString _sessionName;
    QString _collectionError;

    // 上次 applyTheme() 采用的主题。paintEvent 里比对当前主题、不一致就重来 ——
    // 与 ElaText 的自愈同理（ElaText.cpp:158），不把配色正确性只押在
    // themeModeChanged 一定按期到达上。
    ElaThemeType::ThemeMode _themeMode;

    // 单调递增 ID 用于区分不同采集；pending 为 0 表示当前没有在途请求。
    quint64 _nextRequestId{1};
    quint64 _pendingRequestId{0};
    // CPU 与网络字段均为远端累计计数，只有相邻样本做差才有实际意义。
    quint64 _previousCpuTotal{0};
    quint64 _previousCpuIdle{0};
    qint64 _previousSampleMs{0};
    QHash<QString, QPair<quint64, quint64>> _previousNetworkBytes;
    QHash<QString, QPair<double, double>> _networkRates;
    QHash<QString, QVector<QPair<double, double>>> _networkHistory;
    bool _hasMetrics{false};

    // 每秒刷新一次；在途请求未完成时会跳过本轮，避免慢服务端出现命令积压。
    static constexpr int RefreshIntervalMs = 1'000;
};
