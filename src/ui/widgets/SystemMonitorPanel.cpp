/**
 * @file SystemMonitorPanel.cpp
 * @brief 卡片式远端系统资源监视面板与分频 SSH 采集。
 *
 * 复用当前终端的 SSH 连接：常驻请求驱动 channel 读取 Linux /proc，独立
 * exec channel 仅在系统详情可见时查询 df。隐藏、折叠、最小化或切换标签时停采。
 */
#include "SystemMonitorPanel.h"
#include "SystemInformationDialog.h"

#include "ElaComboBox.h"
#include "ElaIconButton.h"
#include "ElaProgressBar.h"
#include "ElaScrollArea.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "service/LanguageManager.h"
#include "service/ResourcePrefetch.h"
#include "transport/SshTransport.h"

#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

constexpr int NetworkHistoryCapacity = 64;
// 与 SessionPanel 一致：控件/分组使用 13 px 基准，详情和列表叶子使用 10 px。
constexpr int PanelFontPixelSize = 13;
constexpr int MetricLabelPixelSize = 13;
constexpr int SecondaryFontPixelSize = 10;

// 普通文字一律用 ElaText，不再手工调 QLabel 的颜色：它自己订阅
// themeModeChanged，并在 paintEvent 里校验 palette 与当前主题是否一致、不一致就
// 重新应用（ElaText.cpp:158）。裸 QLabel 靠祖先 palette 继承取色，主题切换时会
// 残留对端主题的颜色 —— 浅色主题下曾出现整片白底白字。
ElaText* createLabel(QWidget* parent, int pixelSize = PanelFontPixelSize,
                     bool bold = false)
{
    auto* text = new ElaText(parent);
    text->setTextStyle(ElaTextType::Body);
    text->setTextPixelSize(pixelSize);
    if (bold) {
        QFont labelFont = text->font();
        labelFont.setBold(true);
        text->setFont(labelFont);
    }
    text->setWordWrap(false);
    return text;
}

QFrame* createSeparator(QWidget* parent)
{
    auto* separator = new QFrame(parent);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setFixedHeight(1);
    return separator;
}

/** 使用 Ela 主题语义色绘制的网络图例文字。 */
class MetricLegendText final : public ElaText
{
public:
    explicit MetricLegendText(ElaThemeType::ThemeColor colorRole,
                              QWidget* parent = nullptr)
        : ElaText(parent)
        , _colorRole(colorRole)
    {
        setTextStyle(ElaTextType::Body);
        setTextPixelSize(SecondaryFontPixelSize);
        setWordWrap(false);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPalette textPalette = palette();
        const QColor color = eTheme->getThemeColor(
            eTheme->getThemeMode(), _colorRole);
        if (textPalette.color(QPalette::WindowText) != color
            || textPalette.color(QPalette::Text) != color) {
            textPalette.setColor(QPalette::WindowText, color);
            textPalette.setColor(QPalette::Text, color);
            setPalette(textPalette);
        }
        QLabel::paintEvent(event);
    }

private:
    ElaThemeType::ThemeColor _colorRole;
};

/** 使用 ELA 主题色绘制的紧凑资源占用条。 */
class MetricProgressBar final : public ElaProgressBar
{
public:
    explicit MetricProgressBar(ElaThemeType::ThemeColor accentRole,
                               QWidget* parent = nullptr)
        : ElaProgressBar(parent)
        , _accentRole(accentRole)
    {
        setRange(0, 100);
        setValue(0);
        setFormat(QStringLiteral("—"));
        // CPU、内存和交换分区占用条统一使用 28 个 Qt 逻辑像素高度。
        setFixedHeight(28);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto mode = eTheme->getThemeMode();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF track = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(Qt::NoPen);
        const auto trackRole = isEnabled()
            ? ElaThemeType::BasicBaseAlpha : ElaThemeType::BasicDisable;
        painter.setBrush(eTheme->getThemeColor(mode, trackRole));
        painter.drawRoundedRect(track, 5, 5);

        if (isEnabled() && value() > minimum()) {
            const qreal ratio = static_cast<qreal>(value() - minimum())
                / static_cast<qreal>(maximum() - minimum());
            QRectF fill = track;
            fill.setWidth(std::max<qreal>(4.0, track.width() * ratio));
            painter.setBrush(eTheme->getThemeColor(mode, _accentRole));
            painter.drawRoundedRect(fill, 5, 5);
        }

        const auto textRole = isEnabled()
            ? ElaThemeType::BasicText : ElaThemeType::BasicTextDisable;
        painter.setPen(eTheme->getThemeColor(mode, textRole));
        painter.drawText(track.adjusted(10, 0, -6, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, text());
    }

private:
    ElaThemeType::ThemeColor _accentRole;
};

using NovaTerm::LinuxResource::NetworkMetric;
using RemoteMetrics = NovaTerm::LinuxResource::Sample;
using NovaTerm::LinuxResource::parseMetrics;

QString formatBytes(double bytes)
{
    // 系统资源数据采用 1024 进位，与 /proc 和 df -k 的计量方式保持一致。
    static constexpr const char* Units[]{"B", "KiB", "MiB", "GiB", "TiB"};
    std::size_t unit = 0;
    while (bytes >= 1024.0 && unit + 1 < std::size(Units)) {
        bytes /= 1024.0;
        ++unit;
    }
    const int precision = bytes >= 100.0 || unit == 0 ? 0 : 1;
    return QStringLiteral("%1 %2")
        .arg(QString::number(bytes, 'f', precision),
             QLatin1String(Units[unit]));
}

void setUsage(ElaProgressBar* bar, ElaText* detail,
              quint64 usedKiB, quint64 totalKiB)
{
    if (totalKiB == 0) {
        bar->setValue(0);
        // Linux 未配置交换分区时总量合法地为 0，应展示真实的 0%，而非“未知”。
        bar->setFormat(QStringLiteral("0%"));
        detail->setText(QStringLiteral("0/0M"));
        return;
    }

    // 先提升到 long double，避免大容量主机上 used * 100 发生整数溢出。
    const int percent = std::clamp(
        static_cast<int>((static_cast<long double>(usedKiB) * 100.0L)
                         / totalKiB),
        0, 100);
    bar->setValue(percent);
    bar->setFormat(QStringLiteral("%1%").arg(percent));
    // 采样单位为 KiB（1024 字节）；M/G 分别简写 MiB/GiB 字节容量。
    // 两项分别选择单位，相同单位只在末尾显示；G 保留两位小数。
    static constexpr quint64 KiBPerMiB = 1024;
    static constexpr quint64 KiBPerGiB = KiBPerMiB * 1024;
    if (totalKiB >= KiBPerGiB) {
        const QString usedText = usedKiB < KiBPerGiB
            ? QStringLiteral("%1M").arg(usedKiB / KiBPerMiB)
            : QString::number(static_cast<double>(usedKiB) / KiBPerGiB, 'f', 2);
        detail->setText(QStringLiteral("%1/%2G")
            .arg(usedText)
            .arg(static_cast<double>(totalKiB) / KiBPerGiB, 0, 'f', 2));
    } else {
        detail->setText(QStringLiteral("%1/%2M")
            .arg(usedKiB / KiBPerMiB).arg(totalKiB / KiBPerMiB));
    }
}

} // namespace

/** 双序列网络速率柱状图：主色表示接收，按压主题色表示发送。 */
class TrafficChart final : public QWidget
{
public:
    explicit TrafficChart(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumHeight(88);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setSamples(QVector<QPair<double, double>> samples)
    {
        _samples = std::move(samples);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto mode = eTheme->getThemeMode();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QRectF canvas = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ElaThemeColor(mode, BasicBaseAlpha));
        painter.drawRoundedRect(canvas, 6, 6);
        if (_samples.isEmpty())
            return;

        double peak = 1.0;
        for (const auto& sample : _samples)
            peak = std::max({peak, sample.first, sample.second});

        const qreal slotWidth = canvas.width() / NetworkHistoryCapacity;
        const qreal seriesWidth = std::max<qreal>(1.0, slotWidth * 0.42);
        const int offset = NetworkHistoryCapacity - _samples.size();
        for (int index = 0; index < _samples.size(); ++index) {
            const auto& sample = _samples[index];
            const qreal x = canvas.left() + (offset + index) * slotWidth;
            const qreal receiveHeight = canvas.height() * sample.first / peak;
            const qreal sendHeight = canvas.height() * sample.second / peak;

            painter.setBrush(ElaThemeColor(mode, PrimaryNormal));
            painter.drawRoundedRect(
                QRectF(x, canvas.bottom() - receiveHeight,
                       seriesWidth, receiveHeight),
                1.5, 1.5);
            painter.setBrush(ElaThemeColor(mode, PrimaryPress));
            painter.drawRoundedRect(
                QRectF(x + seriesWidth, canvas.bottom() - sendHeight,
                       seriesWidth, sendHeight),
                1.5, 1.5);
        }
    }

private:
    QVector<QPair<double, double>> _samples;
};

SystemMonitorPanel::SystemMonitorPanel(QWidget* parent)
    : QWidget(parent)
    , _themeMode(eTheme->getThemeMode())
{
    setMinimumWidth(260);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    setAutoFillBackground(false);
    // 不再把整个面板统一压到小字号；沿用 SessionPanel 的两级字体策略。
    QFont panelFont = font();
    panelFont.setPixelSize(PanelFontPixelSize);
    QFont secondaryFont = font();
    secondaryFont.setPixelSize(SecondaryFontPixelSize);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(1, 1, 1, 1);
    outerLayout->setSpacing(0);

    auto* scrollArea = new ElaScrollArea(this);
    _scrollArea = scrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // ElaScrollArea 默认隐藏两个滚动条；监控内容高于视口，必须恢复竖直滚动条。
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setAutoFillBackground(false);
    scrollArea->viewport()->setAutoFillBackground(false);
    scrollArea->viewport()->installEventFilter(this);
    outerLayout->addWidget(scrollArea);

    auto* content = new QWidget(scrollArea);
    content->setAutoFillBackground(false);
    scrollArea->setWidget(content);
    content->installEventFilter(this);
    auto* rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(10);

    // 会话名称由停靠标题展示，信息图标放在 CPU 行最右侧。
    auto* infoButton = new ElaIconButton(
        ElaIconType::CircleInfo, 14, 28, 28, content);
    infoButton->setFocusPolicy(Qt::StrongFocus);
    _infoButton = infoButton;

    _availabilityLabel = createLabel(content);
    _availabilityLabel->setWordWrap(true);
    rootLayout->addWidget(_availabilityLabel);
    // 移除重复的“服务器资源”标题和未实现的进程入口，资源指标直接展示。
    auto* resourceGrid = new QGridLayout;
    resourceGrid->setHorizontalSpacing(10);
    // 三条占用条作为同一指标组紧凑排列；4 px 与会话面板内部的密集数据节奏
    // 一致，同时仍保留清晰的行边界。
    resourceGrid->setVerticalSpacing(4);
    resourceGrid->setColumnStretch(1, 1);
    _cpuLabel = createLabel(content, MetricLabelPixelSize);
    _memoryLabel = createLabel(content, MetricLabelPixelSize);
    _swapLabel = createLabel(content, MetricLabelPixelSize);
    _cpuProgress = new MetricProgressBar(
        ElaThemeType::PrimaryNormal, content);
    _memoryProgress = new MetricProgressBar(
        ElaThemeType::PrimaryHover, content);
    _swapProgress = new MetricProgressBar(
        ElaThemeType::BasicIndicator, content);
    for (ElaProgressBar* progress : {
             _cpuProgress, _memoryProgress, _swapProgress}) {
        progress->setFont(secondaryFont);
    }
    _memoryDetail = createLabel(content, SecondaryFontPixelSize);
    _swapDetail = createLabel(content, SecondaryFontPixelSize);
    for (ElaText* detail : {_memoryDetail, _swapDetail})
        detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    resourceGrid->addWidget(_cpuLabel, 0, 0);
    resourceGrid->addWidget(_cpuProgress, 0, 1);
    resourceGrid->addWidget(_infoButton, 0, 2, Qt::AlignRight | Qt::AlignVCenter);
    resourceGrid->addWidget(_memoryLabel, 1, 0);
    resourceGrid->addWidget(_memoryProgress, 1, 1);
    resourceGrid->addWidget(_memoryDetail, 1, 2);
    resourceGrid->addWidget(_swapLabel, 2, 0);
    resourceGrid->addWidget(_swapProgress, 2, 1);
    resourceGrid->addWidget(_swapDetail, 2, 2);
    rootLayout->addLayout(resourceGrid);
    rootLayout->addWidget(createSeparator(content));

    auto* networkHeader = new QHBoxLayout;
    networkHeader->setSpacing(7);
    _receiveLabel = new MetricLegendText(
        ElaThemeType::PrimaryNormal, content);
    _sendLabel = new MetricLegendText(
        ElaThemeType::PrimaryPress, content);
    _interfaceCombo = new ElaComboBox(content);
    _interfaceCombo->setFont(panelFont);
    _interfaceCombo->setMinimumWidth(82);
    // 与上方资源占用条保持相同高度，避免下拉框在紧凑面板中显得过高。
    _interfaceCombo->setFixedHeight(28);
    _interfaceCombo->setSizeAdjustPolicy(ElaComboBox::AdjustToContents);
    networkHeader->addWidget(_receiveLabel);
    networkHeader->addWidget(_sendLabel);
    networkHeader->addStretch();
    networkHeader->addWidget(_interfaceCombo);
    rootLayout->addLayout(networkHeader);

    _trafficChart = new TrafficChart(content);
    rootLayout->addWidget(_trafficChart);

    connect(_interfaceCombo,
            QOverload<int>::of(&ElaComboBox::currentIndexChanged),
            this, [this](int) { updateNetworkView(); });
    connect(_infoButton, &ElaIconButton::clicked,
            this, &SystemMonitorPanel::showSystemInformation);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode) { applyTheme(); });

    _fastTimer = new QTimer(this);
    _fastTimer->setInterval(DefaultFastIntervalMs);
    _fastTimer->setTimerType(Qt::CoarseTimer);
    connect(_fastTimer, &QTimer::timeout,
            this, &SystemMonitorPanel::requestFastMetrics);

    _fileSystemTimer = new QTimer(this);
    _fileSystemTimer->setSingleShot(true);
    _fileSystemTimer->setTimerType(Qt::CoarseTimer);
    connect(_fileSystemTimer, &QTimer::timeout,
            this, &SystemMonitorPanel::requestFileSystems);

    retranslateUi();
    applyTheme();
    refreshAvailability();
}

SystemMonitorPanel::~SystemMonitorPanel()
{
    if (_systemInformationDialog)
        _systemInformationDialog->close();
    if (!_sshTransport)
        return;
    _sshTransport->stopResourceMonitoring();
    if (_pendingFileSystemRequestId != 0)
        _sshTransport->cancelCommand(_pendingFileSystemRequestId);
}

QSize SystemMonitorPanel::sizeHint() const
{
    QSize preferred = QWidget::sizeHint();
    preferred.setHeight(maximumHeight());
    return preferred;
}

void SystemMonitorPanel::updateContentHeight()
{
    if (!_scrollArea || !_scrollArea->widget())
        return;
    QWidget* const content = _scrollArea->widget();
    content->layout()->activate();
    // 状态文字会随宽度换行，必须按真实可用宽度计算，不能固定面板高度。
    const QMargins margins = layout()->contentsMargins();
    const int frame = 2 * _scrollArea->frameWidth();
    const int contentWidth = std::max(1, width() - margins.left() - margins.right() - frame);
    const int contentHeight = content->hasHeightForWidth()
        ? content->heightForWidth(contentWidth) : content->sizeHint().height();
    const int preferredHeight = contentHeight + margins.top() + margins.bottom() + frame;
    if (maximumHeight() != preferredHeight) {
        setMaximumHeight(preferredHeight);
        updateGeometry();
    }
}

void SystemMonitorPanel::setSessionContext(const QString& sessionLabel,
                                           SshTransport* transport)
{
    if (transport && _sshTransport == transport && _sessionName == sessionLabel
        && _connectionGeneration == transport->connectionGeneration())
        return;

    if (_systemInformationDialog) {
        _systemInformationDialog->close();
        _systemInformationDialog = nullptr;
    }
    if (_sshTransport) {
        _sshTransport->stopResourceMonitoring();
        if (_pendingFileSystemRequestId != 0)
            _sshTransport->cancelCommand(_pendingFileSystemRequestId);
        disconnect(_sshTransport, nullptr, this, nullptr);
    }
    _fastTimer->stop();
    _fileSystemTimer->stop();
    _samplingActive = false;
    _fileSystemSamplingActive = false;
    _sessionName = sessionLabel;
    _sshTransport = transport;
    resetMetrics();

    if (_sshTransport) {
        _connectionGeneration = _sshTransport->connectionGeneration();
        // 详情（系统信息对话框）交给连接绑定的分批预取：首批概览先行，其余按
        // 调度均匀铺开；常驻指标持续刷新，df 仅在详情可见时按需采集。
        auto* const prefetch = NovaTerm::LinuxResource::ResourcePrefetch::forTransport(
            _sshTransport);
        if (_prefetch != prefetch) {
            _prefetch = prefetch;
            // 预取对象归 transport 所有；销毁时置空，避免借用指针悬垂。
            connect(_prefetch, &QObject::destroyed, this,
                    [this] { _prefetch = nullptr; });
        }
        // 捕获当前 transport，并在回调中复核，屏蔽切换会话后迟到的异步结果。
        SshTransport* const current = _sshTransport.data();
        connect(current, &ITransport::connected, this, [this, current]() {
            if (_sshTransport == current) {
                setSessionContext(_sessionName, current);
                // 重连等待期可能已同步代际；即使上下文相同也必须恢复采样。
                updateSamplingState();
                refreshAvailability();
            }
        });
        connect(current, &SshTransport::commandFinished, this,
                [this, current](quint64 requestId, const QByteArray& output,
                                const QByteArray&,
                                const QString& errorMessage) {
            if (_sshTransport == current && current->isConnected()
                && _connectionGeneration == current->connectionGeneration()) {
                handleFileSystems(requestId, output, errorMessage);
            }
        });
        connect(current, &SshTransport::resourceSampleFinished, this,
                [this, current](quint64 requestId, const QByteArray& payload,
                                const QString& errorMessage) {
            if (_sshTransport == current && current->isConnected()
                && _connectionGeneration == current->connectionGeneration())
                handleFastMetrics(requestId, payload, errorMessage);
        });
        connect(current, &ITransport::disconnected, this, [this, current]() {
            if (_sshTransport == current)
                setSessionContext(_sessionName, nullptr);
        });
        connect(current, &QObject::destroyed, this, [this, current]() {
            if (_sshTransport == current)
                setSessionContext(_sessionName, nullptr);
        });
    } else {
        // 切到非 SSH 上下文：预取归上一条连接所有，这里只解除引用。
        _prefetch = nullptr;
    }
    updateSamplingState();
    refreshAvailability();
}

void SystemMonitorPanel::showSystemInformation()
{
    if (!_sshTransport || !_sshTransport->isConnected())
        return;
    if (_systemInformationDialog) {
        _systemInformationDialog->showNormal();
        _systemInformationDialog->raise();
        _systemInformationDialog->activateWindow();
        updateSamplingState();
        return;
    }

    QWidget* const dialogParent = window();
    _systemInformationDialog = new SystemInformationDialog(
        _sessionName, dialogParent);
    _systemInformationDialog->installEventFilter(this);
    connect(_systemInformationDialog, &QObject::destroyed, this,
            [this, dialog = _systemInformationDialog.data()]() {
        if (!_systemInformationDialog || _systemInformationDialog == dialog) {
            _systemInformationDialog = nullptr;
            updateSamplingState();
        }
    });
    _systemInformationDialog->show();
    _systemInformationDialog->moveToCenter();
    updateSamplingState();
    updateInformationDialog();
}

void SystemMonitorPanel::setPresentationActive(bool active)
{
    if (_presentationActive == active)
        return;
    _presentationActive = active;
    updateSamplingState();
}

void SystemMonitorPanel::retranslateUi()
{
    _infoButton->setToolTip(tr("System information"));
    _infoButton->setAccessibleName(tr("Open system information"));
    _cpuLabel->setText(tr("CPU"));
    _memoryLabel->setText(tr("Memory"));
    _swapLabel->setText(tr("Swap"));
    _trafficChart->setAccessibleName(tr("Network traffic history"));
    refreshAvailability();
    updateNetworkView();
}

void SystemMonitorPanel::applyTheme()
{
    const auto mode = eTheme->getThemeMode();
    _themeMode = mode;

    // 普通文字由 ElaText 跟随主题；网络图例子类按各自语义色自行绘制。
    _receiveLabel->update();
    _sendLabel->update();

    const QColor separator = ElaThemeColor(mode, BasicBorder);
    const auto separators = findChildren<QFrame*>(QString{},
                                                   Qt::FindChildrenRecursively);
    for (QFrame* frame : separators) {
        if (frame->frameShape() != QFrame::HLine)
            continue;
        QPalette palette = frame->palette();
        palette.setColor(QPalette::WindowText, separator);
        palette.setColor(QPalette::Dark, separator);
        frame->setPalette(palette);
    }

    update();
    _cpuProgress->update();
    _memoryProgress->update();
    _swapProgress->update();
    _trafficChart->update();
}

void SystemMonitorPanel::refreshAvailability()
{
    const bool connected = _sshTransport && _sshTransport->isConnected();
    _infoButton->setEnabled(connected);
    for (QWidget* widget : QList<QWidget*>{
             _cpuProgress, _memoryProgress, _swapProgress,
             _interfaceCombo, _trafficChart}) {
        widget->setEnabled(connected);
    }

    // 与 SFTP 面板一致，通过标题属性发布当前连接信息。
    setWindowTitle(_sessionName.isEmpty()
        ? tr("No active SSH session") : _sessionName);
    if (!connected) {
        _availabilityLabel->setText(
            tr("Select a connected SSH terminal to inspect remote resources."));
        _availabilityLabel->show();
    } else if (!_collectionError.isEmpty()) {
        _availabilityLabel->setText(_collectionError);
        _availabilityLabel->show();
    } else if (!_hasMetrics) {
        _availabilityLabel->setText(tr("Collecting remote resources…"));
        _availabilityLabel->show();
    } else {
        _availabilityLabel->hide();
    }
    updateContentHeight();
}

void SystemMonitorPanel::updateSamplingState()
{
    const bool detailsVisible = _systemInformationDialog
        && _systemInformationDialog->isVisible()
        && !_systemInformationDialog->isMinimized();
    const bool shouldSample = _presentationActive
        && (isVisible() || detailsVisible)
        && _sshTransport && _sshTransport->isConnected();
    // 详情开关与面板快采独立，必须在快采状态未变化的提前返回之前处理。
    const bool shouldQueryFileSystems = shouldSample && detailsVisible;
    if (_fileSystemSamplingActive != shouldQueryFileSystems) {
        _fileSystemSamplingActive = shouldQueryFileSystems;
        if (_fileSystemSamplingActive) {
            _hasFileSystemResult = false;
            _fileSystemTimer->start(0);
        } else {
            _fileSystemTimer->stop();
            const quint64 requestId = std::exchange(_pendingFileSystemRequestId, 0);
            if (_sshTransport && requestId != 0)
                _sshTransport->cancelCommand(requestId);
        }
    }
    if (_samplingActive == shouldSample)
        return;

    _samplingActive = shouldSample;
    if (!_samplingActive) {
        _fastTimer->stop();
        if (_sshTransport)
            _sshTransport->stopResourceMonitoring();
        _pendingFastRequestId = 0;
        // 暂停期间远端累计计数继续变化；恢复时必须重新建立差分基线。
        _previousCpuTotal = 0;
        _previousCpuIdle = 0;
        _previousSampleElapsedMs = -1;
        _previousNetworkBytes.clear();
        _networkRates.clear();
        _hasCpuBaseline = false;
        _warmupIntervals = 0;
        _cpuUsage.clear();
        updateNetworkView();
        return;
    }

    _sampleClock.restart();
    _sshTransport->startResourceMonitoring();
    _fastTimer->start();
    QTimer::singleShot(0, this, &SystemMonitorPanel::requestFastMetrics);
}

void SystemMonitorPanel::requestFastMetrics()
{
    if (_sshTransport && _connectionGeneration != _sshTransport->connectionGeneration()) {
        setSessionContext(_sessionName, _sshTransport);
        return;
    }
    // 只提交面板自己需要的指标；详情由 ResourcePrefetch 分批推进，不在这里触发。
    // 最多一个在途请求；慢服务端不会积压定时任务。
    if (!_samplingActive || !_sshTransport
        || !_sshTransport->isConnected() || _pendingFastRequestId != 0) {
        return;
    }
    const quint64 requestId = _nextRequestId++;
    if (_sshTransport->requestResourceSample(requestId)) {
        _pendingFastRequestId = requestId;
        if (!_hasMetrics) {
            _collectionError.clear();
            refreshAvailability();
        }
    }
}

void SystemMonitorPanel::requestFileSystems()
{
    if (!_fileSystemSamplingActive || !_samplingActive || !_sshTransport
        || !_systemInformationDialog || !_systemInformationDialog->isVisible()
        || _systemInformationDialog->isMinimized()
        || !_sshTransport->isConnected()
        || _connectionGeneration != _sshTransport->connectionGeneration()
        || _pendingFileSystemRequestId != 0) {
        return;
    }
    // 文件系统只服务详情窗口，仍须让开静态预取，避免叠加远端负载。
    if (_prefetch && !_prefetch->allowsSlowQuery()) {
        _fileSystemTimer->start(FileSystemDeferralMs);
        return;
    }
    const quint64 requestId = _nextRequestId++;
    // CPU 频率由详情预取（该批同时读 cpuinfo）负责，这里只查文件系统。
    if (_sshTransport->executeCommand(requestId,
            NovaTerm::LinuxResource::slowCommand(/*includeFrequency=*/false))) {
        _pendingFileSystemRequestId = requestId;
    } else {
        _fileSystemTimer->start(FileSystemDeferralMs);
    }
}

void SystemMonitorPanel::updateInformationDialog()
{
    if (!_systemInformationDialog)
        return;
    // 静态详情可能仍在分批到达；对话框按已有分节渲染，缺的卡片标为采集中。
    const QByteArray staticRaw = _prefetch ? _prefetch->data() : QByteArray{};
    _systemInformationDialog->populate(
        NovaTerm::LinuxResource::information(
            staticRaw, _slowInformation, _latestSample, _cpuUsage),
        !_prefetch || !_prefetch->complete()
            || (_fileSystemSamplingActive && !_hasFileSystemResult));
}

void SystemMonitorPanel::handleFastMetrics(
    quint64 requestId, const QByteArray& payload, const QString& errorMessage)
{
    // 会话切换或重置后 pending ID 会清零，因此旧会话的迟到结果会被忽略。
    if (requestId == 0 || requestId != _pendingFastRequestId)
        return;
    _pendingFastRequestId = 0;

    RemoteMetrics metrics;
    if (!errorMessage.isEmpty() || !parseMetrics(payload, metrics)) {
        const QString details = errorMessage;
        _collectionError = details.isEmpty()
            ? tr("The remote system did not return supported Linux metrics.")
            : tr("Remote resource query failed: %1").arg(details);
        refreshAvailability();
        return;
    }
    _collectionError.clear();

    _cpuUsage = _hasCpuBaseline
        ? NovaTerm::LinuxResource::cpuUsage(metrics.ticks, _latestSample.ticks)
        : QByteArray{};
    _latestSample = metrics;
    _hasCpuBaseline = true;

    // 连接后的前两个采样区间属于预热：登录 shell 启动、常驻通道建立与
    // 概览批都落在其中。实测（单核设备，每条远端命令约 10ms CPU）这些一次性开销
    // 可达数个百分点，若计入显示值会被误读成远端持续占用 —— 预热期只更新基线，
    // 不发布 CPU/网络读数（内存与交换是绝对值，照常显示）。
    const bool warmup = _warmupIntervals < WarmupIntervals;
    if (warmup)
        ++_warmupIntervals;

    const qint64 nowMs = _sampleClock.elapsed();
    // /proc/stat 是开机以来的累计 tick；首个样本仅建立基线，后续才可计算占用率。
    if (!warmup && !_cpuUsage.isEmpty() && _previousCpuTotal > 0
        && metrics.cpuTotal > _previousCpuTotal) {
        const quint64 totalDelta = metrics.cpuTotal - _previousCpuTotal;
        const quint64 idleDelta = metrics.cpuIdle >= _previousCpuIdle
            ? metrics.cpuIdle - _previousCpuIdle : 0;
        const int cpuPercent = std::clamp(
            static_cast<int>(((totalDelta - std::min(idleDelta, totalDelta))
                              * 100ULL) / totalDelta),
            0, 100);
        _cpuProgress->setValue(cpuPercent);
        _cpuProgress->setFormat(QStringLiteral("%1%").arg(cpuPercent));
    } else {
        _cpuProgress->setValue(0);
        _cpuProgress->setFormat(tr("Collecting…"));
    }
    _previousCpuTotal = metrics.cpuTotal;
    _previousCpuIdle = metrics.cpuIdle;

    const quint64 memoryUsed = metrics.memoryTotalKiB
        - std::min(metrics.memoryAvailableKiB, metrics.memoryTotalKiB);
    setUsage(_memoryProgress, _memoryDetail,
             memoryUsed, metrics.memoryTotalKiB);
    const quint64 swapUsed = metrics.swapTotalKiB
        - std::min(metrics.swapFreeKiB, metrics.swapTotalKiB);
    setUsage(_swapProgress, _swapDetail, swapUsed, metrics.swapTotalKiB);

    // 使用真实采样间隔而非固定 1 秒，兼容定时器抖动和远端命令执行耗时。
    const double elapsedSeconds = _previousSampleElapsedMs >= 0
        ? static_cast<double>(nowMs - _previousSampleElapsedMs) / 1000.0
        : 0.0;
    QHash<QString, QPair<quint64, quint64>> currentNetworkBytes;
    QHash<QString, QPair<double, double>> currentRates;
    QStringList interfaceNames;
    for (const NetworkMetric& metric : metrics.networks) {
        interfaceNames.append(metric.name);
        currentNetworkBytes.insert(
            metric.name, {metric.receivedBytes, metric.sentBytes});
        const auto previous = _previousNetworkBytes.constFind(metric.name);
        if (elapsedSeconds <= 0.0 || previous == _previousNetworkBytes.cend())
            continue;

        // 计数器回绕或网卡重置时将本次差值视为 0，避免图表出现异常尖峰。
        const quint64 receivedDelta = metric.receivedBytes >= previous->first
            ? metric.receivedBytes - previous->first : 0;
        const quint64 sentDelta = metric.sentBytes >= previous->second
            ? metric.sentBytes - previous->second : 0;
        const QPair<double, double> rate{
            static_cast<double>(receivedDelta) / elapsedSeconds,
            static_cast<double>(sentDelta) / elapsedSeconds};
        currentRates.insert(metric.name, rate);
        auto& history = _networkHistory[metric.name];
        history.append(rate);
        if (history.size() > NetworkHistoryCapacity)
            history.remove(0, history.size() - NetworkHistoryCapacity);
    }
    _previousNetworkBytes = std::move(currentNetworkBytes);
    _networkRates = std::move(currentRates);

    const QString selectedInterface = _interfaceCombo->currentText();
    {
        const QSignalBlocker blocker(_interfaceCombo);
        _interfaceCombo->clear();
        _interfaceCombo->addItems(interfaceNames);
        const int previousIndex = interfaceNames.indexOf(selectedInterface);
        if (previousIndex >= 0)
            _interfaceCombo->setCurrentIndex(previousIndex);
    }
    updateNetworkView();

    _previousSampleElapsedMs = nowMs;
    _hasMetrics = true;
    refreshAvailability();
    updateInformationDialog();
}

void SystemMonitorPanel::handleFileSystems(
    quint64 requestId, const QByteArray& standardOutput,
    const QString& errorMessage)
{
    if (requestId == 0 || requestId != _pendingFileSystemRequestId)
        return;
    _pendingFileSystemRequestId = 0;

    _hasFileSystemResult = true;
    // 失败时保留最近一次有效结果；首查失败则结束“采集中”，由详情显示空状态。
    if (errorMessage.isEmpty()
        && !NovaTerm::LinuxResource::fileSystems(standardOutput).isEmpty()) {
        _slowInformation = standardOutput;
    }
    updateInformationDialog();
    if (_fileSystemSamplingActive)
        _fileSystemTimer->start(FileSystemIntervalMs);
}

void SystemMonitorPanel::updateNetworkView()
{
    // 以远端网卡为准：first 为接收（下行），second 为发送（上行）。
    const QString interfaceName = _interfaceCombo->currentText();
    const auto rate = _networkRates.constFind(interfaceName);
    if (rate == _networkRates.cend()) {
        _receiveLabel->setText(tr("↓ —"));
        _sendLabel->setText(tr("↑ —"));
    } else {
        _receiveLabel->setText(
            tr("↓ %1/s").arg(formatBytes(rate->first)));
        _sendLabel->setText(
            tr("↑ %1/s").arg(formatBytes(rate->second)));
    }
    _trafficChart->setSamples(_networkHistory.value(interfaceName));
}

void SystemMonitorPanel::resetMetrics()
{
    // transport 上下文变化后累计计数不可跨主机比较，必须连同请求状态一起清空。
    _pendingFastRequestId = 0;
    _pendingFileSystemRequestId = 0;
    _slowInformation.clear();
    _latestSample = {};
    _cpuUsage.clear();
    _hasCpuBaseline = false;
    _warmupIntervals = 0;
    _previousCpuTotal = 0;
    _previousCpuIdle = 0;
    _previousSampleElapsedMs = -1;
    _previousNetworkBytes.clear();
    _networkRates.clear();
    _networkHistory.clear();
    _collectionError.clear();
    _hasMetrics = false;
    _hasFileSystemResult = false;
    _interfaceCombo->clear();
    _trafficChart->setSamples({});
    for (ElaProgressBar* bar : {
             _cpuProgress, _memoryProgress, _swapProgress}) {
        bar->setValue(0);
        bar->setFormat(QStringLiteral("—"));
    }
    _memoryDetail->setText(QStringLiteral("—"));
    _swapDetail->setText(QStringLiteral("—"));
    updateNetworkView();
}

void SystemMonitorPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    updateSamplingState();
}

void SystemMonitorPanel::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateSamplingState();
}

bool SystemMonitorPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (_scrollArea
        && ((watched == _scrollArea->viewport() && event->type() == QEvent::Resize)
            || (watched == _scrollArea->widget() && event->type() == QEvent::LayoutRequest))) {
        QTimer::singleShot(0, this, &SystemMonitorPanel::updateContentHeight);
    }
    if (watched == _systemInformationDialog
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowStateChange)) {
        // 等可见性与窗口状态更新完成后再启停采集；上下文销毁会取消回调。
        QTimer::singleShot(0, this, &SystemMonitorPanel::updateSamplingState);
    }
    return QWidget::eventFilter(watched, event);
}

void SystemMonitorPanel::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    const auto mode = eTheme->getThemeMode();
    // 自愈：themeModeChanged 已连到 applyTheme()，但那条通路一旦没按期到达，
    // 手工上色的那几处就会残留对端主题的颜色。这里比对一次再补，逻辑与
    // ElaText.cpp:158 相同；_themeMode 先更新，因此不会与 update() 互相触发。
    if (mode != _themeMode)
        applyTheme();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ElaThemeColor(mode, BasicBorder), 1));
    painter.setBrush(ElaThemeColor(mode, DialogBase));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            8, 8);
}
