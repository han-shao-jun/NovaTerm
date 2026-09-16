/**
 * @file SystemMonitorPanel.h
 * @brief 可停靠的卡片式远端系统资源监视面板。
 */
#pragma once

#include "ElaDef.h"
#include "service/LinuxResourceData.h"

#include <QHash>
#include <QElapsedTimer>
#include <QPair>
#include <QPointer>
#include <QVector>
#include <QWidget>

class ElaComboBox;
class ElaIconButton;
class ElaProgressBar;
class ElaScrollArea;
class ElaText;
class QEvent;
class QPaintEvent;
class QHideEvent;
class QShowEvent;
class QTimer;
class SshTransport;
class SystemInformationDialog;
class TrafficChart;

namespace NovaTerm::LinuxResource {
class ResourcePrefetch;
}

class SystemMonitorPanel final : public QWidget
{
    Q_OBJECT

public:
    explicit SystemMonitorPanel(QWidget* parent = nullptr);
    ~SystemMonitorPanel() override;

    [[nodiscard]] QSize sizeHint() const override;

    /** 更新当前终端标签及其已连接的 SSH transport。 */
    void setSessionContext(const QString& sessionLabel, SshTransport* transport);
    /** 主窗口最小化时关闭，恢复时重新按控件可见性决定是否采样。 */
    void setPresentationActive(bool active);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void retranslateUi();
    void applyTheme();
    /** 按当前宽度与状态提示计算内容高度，限制停靠面板的多余留白。 */
    void updateContentHeight();
    void showSystemInformation();
    void refreshAvailability();
    void updateSamplingState();
    /** 提交一次有界、非重入的快速资源采集请求（仅面板需要的 CPU/内存/网络）。 */
    void requestFastMetrics();
    /** 仅在详情窗口可见时查询文件系统；详情预取在途时让路。 */
    void requestFileSystems();
    void updateInformationDialog();
    /** 校验请求归属，解析结果并用相邻样本计算 CPU/网络速率。 */
    void handleFastMetrics(quint64 requestId, const QByteArray& payload,
                           const QString& errorMessage);
    void handleFileSystems(quint64 requestId,
                           const QByteArray& standardOutput,
                           const QString& errorMessage);
    void updateNetworkView();
    /** 切换或断开会话时清除所有累计值基线。 */
    void resetMetrics();

    ElaScrollArea* _scrollArea{nullptr};
    ElaText* _availabilityLabel{nullptr};
    ElaIconButton* _infoButton{nullptr};
    ElaText* _cpuLabel{nullptr};
    ElaText* _memoryLabel{nullptr};
    ElaText* _swapLabel{nullptr};
    ElaProgressBar* _cpuProgress{nullptr};
    ElaProgressBar* _memoryProgress{nullptr};
    ElaProgressBar* _swapProgress{nullptr};
    ElaText* _memoryDetail{nullptr};
    ElaText* _swapDetail{nullptr};
    ElaText* _receiveLabel{nullptr};
    ElaText* _sendLabel{nullptr};
    ElaComboBox* _interfaceCombo{nullptr};
    TrafficChart* _trafficChart{nullptr};
    QTimer* _fastTimer{nullptr};
    QTimer* _fileSystemTimer{nullptr};
    QPointer<SshTransport> _sshTransport;
    QPointer<SystemInformationDialog> _systemInformationDialog;
    QString _sessionName;
    QString _collectionError;

    // 上次 applyTheme() 采用的主题。paintEvent 里比对当前主题、不一致就重来，
    // 不把面板自绘配色正确性只押在 themeModeChanged 一定按期到达上。
    ElaThemeType::ThemeMode _themeMode;

    // 单调递增 ID 用于区分不同采集；pending 为 0 表示当前没有在途请求。
    quint64 _nextRequestId{1};
    quint64 _pendingFastRequestId{0};
    quint64 _pendingFileSystemRequestId{0};
    quint64 _connectionGeneration{0};
    // 系统信息对话框的静态详情由连接绑定的分批预取持有：切换标签不重启采集，
    // 回连后按新代际重来（见 service/ResourcePrefetch.h）。对象归 transport 所有，
    // 这里只借用，并连 destroyed 兜底置空。
    NovaTerm::LinuxResource::ResourcePrefetch* _prefetch{nullptr};
    QByteArray _slowInformation;
    QByteArray _cpuUsage;
    NovaTerm::LinuxResource::Sample _latestSample;
    bool _hasCpuBaseline{false};
    /** 已计入的采样区间数；小于 WarmupIntervals 时不发布读数。 */
    int _warmupIntervals{0};
    // CPU 与网络字段均为远端累计计数，只有相邻样本做差才有实际意义。
    quint64 _previousCpuTotal{0};
    quint64 _previousCpuIdle{0};
    QElapsedTimer _sampleClock;
    qint64 _previousSampleElapsedMs{-1};
    QHash<QString, QPair<quint64, quint64>> _previousNetworkBytes;
    QHash<QString, QPair<double, double>> _networkRates;
    QHash<QString, QVector<QPair<double, double>>> _networkHistory;
    bool _hasMetrics{false};
    bool _hasFileSystemResult{false};
    bool _presentationActive{true};
    bool _samplingActive{false};
    bool _fileSystemSamplingActive{false};

    // Fast 每秒采样；Slow 仅在详情可见时每 10 秒查询，Static 按连接分批预取。
    static constexpr int DefaultFastIntervalMs = 1'000;
    static constexpr int FileSystemIntervalMs = 10'000;
    /** 详情预取占用时间片时，文件系统查询的退避重试间隔。 */
    static constexpr int FileSystemDeferralMs = 250;
    /**
     * 连接后的前两个 1 秒区间作为预热：登录 shell 启动、常驻通道建立
     * 与概览批都落在其中，一次性开销可达数个百分点，不代表远端稳态负载。
     * 预热期只更新差分基线，不发布 CPU/网络读数。
     */
    static constexpr int WarmupIntervals = 2;
};
