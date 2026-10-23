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
    /** 会话页面隐藏时仍采样；由外层 Dock 统一控制最小化/关闭时的暂停。 */
    void setBackgroundSamplingEnabled(bool enabled);

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
    /** 提交一个有界采样请求；详情可见时在同一常驻通道附加磁盘原始数据。 */
    void requestFastMetrics();
    void updateInformationDialog();
    /** 校验请求归属，解析结果并用相邻样本计算 CPU/网络速率。 */
    void handleFastMetrics(quint64 requestId, const QByteArray& payload,
                           const QString& errorMessage);
    void updateNetworkView();
    /** 绑定不同 transport 或断开重连时清除累计值基线；切换页面不调用。 */
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
    bool _pendingSampleIncludesFileSystems{false};
    quint64 _connectionGeneration{0};
    // 系统信息对话框的静态详情由连接绑定的分批预取持有：切换标签不重启采集，
    // 回连后按新代际重来（见 service/ResourcePrefetch.h）。对象归 transport 所有，
    // 这里只借用，并连 destroyed 兜底置空。
    NovaTerm::LinuxResource::ResourcePrefetch* _prefetch{nullptr};
    QByteArray _slowInformation;
    QByteArray _cpuUsage;
    NovaTerm::LinuxResource::Sample _latestSample;
    bool _hasCpuBaseline{false};
    // CPU 与网络字段均为远端累计计数，只有相邻样本做差才有实际意义。
    quint64 _previousCpuTotal{0};
    quint64 _previousCpuIdle{0};
    QElapsedTimer _sampleClock;
    qint64 _previousSampleElapsedMs{-1};
    qint64 _lastFileSystemSampleElapsedMs{-1};
    QHash<QString, QPair<quint64, quint64>> _previousNetworkBytes;
    QHash<QString, QPair<double, double>> _networkRates;
    QHash<QString, QVector<QPair<double, double>>> _networkHistory;
    bool _hasMetrics{false};
    bool _hasFileSystemResult{false};
    bool _presentationActive{true};
    bool _backgroundSamplingEnabled{false};
    bool _samplingActive{false};
    bool _fileSystemSamplingActive{false};

    // 初始化按完成推进；之后保持原快速指标/磁盘刷新周期。
    static constexpr int DefaultFastIntervalMs = 1'000;
    static constexpr int FileSystemIntervalMs = 10'000;
    static constexpr int ResourceRetryDelayMs = 1'000;
};
