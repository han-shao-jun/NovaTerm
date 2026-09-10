/**
 * @file SystemMonitorPanel.h
 * @brief 可停靠的卡片式远端系统资源监视面板。
 */
#pragma once

#include "ElaDef.h"

#include <QHash>
#include <QElapsedTimer>
#include <QPair>
#include <QPointer>
#include <QVector>
#include <QWidget>

class ElaTreeWidget;
class ElaText;
class QLabel;
class QComboBox;
class QPaintEvent;
class QHideEvent;
class QShowEvent;
class QProgressBar;
class QPushButton;
class QTimer;
class SshTransport;
class SystemInformationDialog;
class TrafficChart;

class SystemMonitorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SystemMonitorPanel(QWidget* parent = nullptr);
    ~SystemMonitorPanel() override;

    /** 更新当前终端标签及其已连接的 SSH transport。 */
    void setSessionContext(const QString& sessionLabel, SshTransport* transport);
    /** 主窗口最小化时关闭，恢复时重新按控件可见性决定是否采样。 */
    void setPresentationActive(bool active);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

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
    void showSystemInformation();
    void refreshAvailability();
    void updateSamplingState();
    /** 提交一次有界、非重入的快速资源采集请求。 */
    void requestFastMetrics();
    /** 提交独立的低频文件系统容量查询。 */
    void requestFileSystems();
    /** 校验请求归属，解析结果并用相邻样本计算 CPU/网络速率。 */
    void handleFastMetrics(quint64 requestId, const QByteArray& payload,
                           const QString& errorMessage);
    void handleFileSystems(quint64 requestId,
                           const QByteArray& standardOutput,
                           const QByteArray& standardError,
                           const QString& errorMessage);
    void updateNetworkView();
    /** 切换或断开会话时清除所有累计值基线。 */
    void resetMetrics();

    ElaText* _availabilityLabel{nullptr};
    QPushButton* _infoButton{nullptr};
    ElaText* _cpuLabel{nullptr};
    ElaText* _memoryLabel{nullptr};
    ElaText* _swapLabel{nullptr};
    QProgressBar* _cpuProgress{nullptr};
    QProgressBar* _memoryProgress{nullptr};
    QProgressBar* _swapProgress{nullptr};
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
    QTimer* _fastTimer{nullptr};
    QTimer* _fileSystemTimer{nullptr};
    QPointer<SshTransport> _sshTransport;
    QPointer<SystemInformationDialog> _systemInformationDialog;
    QString _sessionName;
    QString _collectionError;
    QString _fileSystemError;

    // 上次 applyTheme() 采用的主题。paintEvent 里比对当前主题、不一致就重来 ——
    // 与 ElaText 的自愈同理（ElaText.cpp:158），不把配色正确性只押在
    // themeModeChanged 一定按期到达上。
    ElaThemeType::ThemeMode _themeMode;

    // 单调递增 ID 用于区分不同采集；pending 为 0 表示当前没有在途请求。
    quint64 _nextRequestId{1};
    quint64 _pendingFastRequestId{0};
    quint64 _pendingFileSystemRequestId{0};
    // CPU 与网络字段均为远端累计计数，只有相邻样本做差才有实际意义。
    quint64 _previousCpuTotal{0};
    quint64 _previousCpuIdle{0};
    QElapsedTimer _sampleClock;
    qint64 _previousSampleElapsedMs{-1};
    QHash<QString, QPair<quint64, quint64>> _previousNetworkBytes;
    QHash<QString, QPair<double, double>> _networkRates;
    QHash<QString, QVector<QPair<double, double>>> _networkHistory;
    bool _hasMetrics{false};
    bool _hasFileSystems{false};
    bool _presentationActive{true};
    bool _samplingActive{false};

    // 快速指标默认 2 秒（配置仅接受 1 或 2 秒），文件系统容量独立低频查询。
    static constexpr int DefaultFastIntervalMs = 2'000;
    static constexpr int FileSystemIntervalMs = 30'000;
};
