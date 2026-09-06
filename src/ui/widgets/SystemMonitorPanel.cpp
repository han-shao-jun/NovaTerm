/**
 * @file SystemMonitorPanel.cpp
 * @brief 卡片式远端系统资源监视面板与分频 SSH 采集。
 *
 * 复用当前终端的 SSH 连接：常驻请求驱动 channel 读取 Linux /proc，独立
 * 低频 exec channel 查询 df。隐藏、折叠、最小化或切换标签时停止快速采样。
 */
#include "SystemMonitorPanel.h"

#include "ElaComboBox.h"
#include "ElaIconButton.h"
#include "ElaScrollBar.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "ElaTreeWidget.h"
#include "service/ConfigManager.h"
#include "service/LanguageManager.h"
#include "transport/SshTransport.h"

#include <QFrame>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QProgressBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

constexpr int NetworkHistoryCapacity = 64;
// 与 SessionPanel 一致：控件/分组使用 13 px 基准，详情和列表叶子使用 10 px。
constexpr int PanelFontPixelSize = 13;
constexpr int MetricLabelPixelSize = 13;
constexpr int SecondaryFontPixelSize = 10;

void setLabelColor(QLabel* label, const QColor& color)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    label->setPalette(palette);
}

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

/** 使用 ELA 主题色绘制的紧凑资源占用条。 */
class MetricProgressBar final : public QProgressBar
{
public:
    explicit MetricProgressBar(ElaThemeType::ThemeColor accentRole,
                               QWidget* parent = nullptr)
        : QProgressBar(parent)
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

struct NetworkMetric
{
    QString name;
    // /proc/net/dev 提供的是启动以来的累计字节数，速率需用相邻采样做差。
    quint64 receivedBytes{0};
    quint64 sentBytes{0};
};

struct FileSystemMetric
{
    QString path;
    // df -k 的容量单位固定为 KiB，展示时再统一换算为可读格式。
    quint64 availableKiB{0};
    quint64 sizeKiB{0};
};

struct RemoteMetrics
{
    quint64 cpuTotal{0};
    quint64 cpuIdle{0};
    quint64 memoryTotalKiB{0};
    quint64 memoryAvailableKiB{0};
    quint64 swapTotalKiB{0};
    quint64 swapFreeKiB{0};
    QList<NetworkMetric> networks;
};

QByteArray fileSystemQueryCommand()
{
    // df 与快速 /proc 通道完全隔离；慢挂载点不会拖住 CPU/内存/网络刷新。
    return QByteArrayLiteral(R"NOVATERM(LC_ALL=C; export LC_ALL
(df -Pk 2>/dev/null || df -k 2>/dev/null) | awk 'NR > 1 { printf "FS\t%s\t%s\t%s\n", $NF, $4, $2 }'
)NOVATERM");
}

bool parseUnsigned(const QByteArray& value, quint64& result)
{
    bool ok = false;
    result = value.toULongLong(&ok);
    return ok;
}

bool parseMetrics(const QByteArray& output, RemoteMetrics& metrics)
{
    // 远端脚本使用“类型 + 制表符字段”的稳定协议，避免依赖本地化输出文本。
    // CPU 和内存是面板的基础指标，缺少任一项即视为本次采集无效。
    bool hasCpu = false;
    bool hasMemory = false;
    constexpr int MaximumInterfaces = 128;
    for (const QByteArray& rawLine : output.split('\n')) {
        const auto fields = rawLine.trimmed().split('\t');
        if (fields.isEmpty())
            continue;

        if (fields[0] == QByteArrayLiteral("CPU") && fields.size() == 3) {
            hasCpu = parseUnsigned(fields[1], metrics.cpuTotal)
                && parseUnsigned(fields[2], metrics.cpuIdle);
        } else if (fields[0] == QByteArrayLiteral("MEM")
                   && fields.size() == 5) {
            hasMemory = parseUnsigned(fields[1], metrics.memoryTotalKiB)
                && parseUnsigned(fields[2], metrics.memoryAvailableKiB)
                && parseUnsigned(fields[3], metrics.swapTotalKiB)
                && parseUnsigned(fields[4], metrics.swapFreeKiB);
        } else if (fields[0] == QByteArrayLiteral("NET")
                   && fields.size() == 4) {
            if (metrics.networks.size() >= MaximumInterfaces)
                return false;
            NetworkMetric metric;
            metric.name = QString::fromUtf8(fields[1]);
            if (parseUnsigned(fields[2], metric.receivedBytes)
                && parseUnsigned(fields[3], metric.sentBytes)) {
                metrics.networks.append(std::move(metric));
            }
        }
    }
    return hasCpu && hasMemory;
}

bool parseFileSystems(const QByteArray& output,
                      QList<FileSystemMetric>& fileSystems)
{
    constexpr int MaximumFileSystems = 128;
    for (const QByteArray& rawLine : output.split('\n')) {
        const auto fields = rawLine.trimmed().split('\t');
        if (fields.isEmpty())
            continue;
        if (fields[0] == QByteArrayLiteral("FS") && fields.size() == 4) {
            if (fileSystems.size() >= MaximumFileSystems)
                return false;
            FileSystemMetric metric;
            metric.path = QString::fromUtf8(fields[1]);
            if (parseUnsigned(fields[2], metric.availableKiB)
                && parseUnsigned(fields[3], metric.sizeKiB)) {
                fileSystems.append(std::move(metric));
            }
        }
    }
    return !fileSystems.isEmpty();
}

QString formatBytes(double bytes)
{
    // 系统资源数据采用 1024 进位，与 /proc 和 df -k 的计量方式保持一致。
    static constexpr const char* Units[]{"B", "KiB", "MiB", "GiB", "TiB"};
    qsizetype unit = 0;
    while (bytes >= 1024.0 && unit + 1 < std::size(Units)) {
        bytes /= 1024.0;
        ++unit;
    }
    const int precision = bytes >= 100.0 || unit == 0 ? 0 : 1;
    return QStringLiteral("%1 %2")
        .arg(QString::number(bytes, 'f', precision),
             QLatin1String(Units[unit]));
}

void setUsage(QProgressBar* bar, QLabel* detail,
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
    // 内存与交换容量统一用整数 MiB，紧凑显示为“已用/总量M”。
    detail->setText(QStringLiteral("%1/%2M")
        .arg(usedKiB / 1024).arg(totalKiB / 1024));
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
    setMinimumSize(260, 360);
    setAutoFillBackground(false);
    // 不再把整个面板统一压到小字号；沿用 SessionPanel 的两级字体策略。
    QFont panelFont = font();
    panelFont.setPixelSize(PanelFontPixelSize);
    QFont secondaryFont = font();
    secondaryFont.setPixelSize(SecondaryFontPixelSize);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(1, 1, 1, 1);
    outerLayout->setSpacing(0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    // 只换滚动条、不换成 ElaScrollArea：后者构造时就把两个方向的 policy 设成
    // ScrollBarAlwaysOff（ElaScrollArea.cpp:18-19），而本面板内容高于视口，
    // 需要一条可见的竖直滚动条。
    scrollArea->setVerticalScrollBar(new ElaScrollBar(scrollArea));
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setAutoFillBackground(false);
    scrollArea->viewport()->setAutoFillBackground(false);
    outerLayout->addWidget(scrollArea);

    auto* content = new QWidget(scrollArea);
    content->setAutoFillBackground(false);
    scrollArea->setWidget(content);
    auto* rootLayout = new QVBoxLayout(content);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(10);

    // 会话名称由停靠标题展示，信息图标放在 CPU 行最右侧。
    auto* infoButton = new ElaIconButton(
        ElaIconType::CircleInfo, 14, 28, 28, content);
    infoButton->setFocusPolicy(Qt::NoFocus);
    infoButton->setAttribute(Qt::WA_TransparentForMouseEvents);
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
    for (QProgressBar* progress : {
             _cpuProgress, _memoryProgress, _swapProgress}) {
        progress->setFont(secondaryFont);
    }
    _memoryDetail = createLabel(content, SecondaryFontPixelSize);
    _swapDetail = createLabel(content, SecondaryFontPixelSize);
    for (QLabel* detail : {_memoryDetail, _swapDetail})
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
    _receiveLabel = createLabel(content, SecondaryFontPixelSize);
    _sendLabel = createLabel(content, SecondaryFontPixelSize);
    _interfaceCombo = new ElaComboBox(content);
    _interfaceCombo->setFont(panelFont);
    _interfaceCombo->setMinimumWidth(82);
    // 与上方资源占用条保持相同高度，避免下拉框在紧凑面板中显得过高。
    _interfaceCombo->setFixedHeight(28);
    _interfaceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    networkHeader->addWidget(_receiveLabel);
    networkHeader->addWidget(_sendLabel);
    networkHeader->addStretch();
    networkHeader->addWidget(_interfaceCombo);
    rootLayout->addLayout(networkHeader);

    _trafficChart = new TrafficChart(content);
    rootLayout->addWidget(_trafficChart);
    rootLayout->addWidget(createSeparator(content));

    auto* fileHeader = new QHBoxLayout;
    _pathHeader = createLabel(content, PanelFontPixelSize, true);
    _capacityHeader = createLabel(content, PanelFontPixelSize, true);
    _capacityHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fileHeader->addWidget(_pathHeader);
    fileHeader->addStretch();
    fileHeader->addWidget(_capacityHeader);
    rootLayout->addLayout(fileHeader);

    _diskTree = new ElaTreeWidget(content);
    _diskTree->setFont(secondaryFont);
    _diskTree->setColumnCount(2);
    _diskTree->setHeaderHidden(true);
    _diskTree->setRootIsDecorated(false);
    _diskTree->setUniformRowHeights(true);
    _diskTree->setIndentation(0);
    _diskTree->setFrameShape(QFrame::NoFrame);
    // NoFrame 只让 frameWidth 归零，挡不住 Qt 向 style 派发 CE_ShapedFrame ——
    // Ela 的树样式在该元素里画圆角边线 + 底色。本列表要融进外层卡片，边界由卡片
    // 自己提供，故显式关掉。
    _diskTree->setIsFrameVisible(false);
    _diskTree->setFocusPolicy(Qt::NoFocus);
    _diskTree->setSelectionMode(QAbstractItemView::NoSelection);
    _diskTree->setMinimumHeight(150);
    // 不能用 setStyleSheet() 收紧行高：那会整体替换 ElaTreeWidget 构造里设的
    // 透明背景 QSS。透明与无边框已由该控件与上面的 NoFrame 提供，行高改用
    // ElaTreeViewStyle 的 ItemHeight 表达 —— 默认 35px 在这个紧凑面板里过高，
    // 单行高度与 SessionPanel 分组行相同，且随实际字体度量增长，避免高 DPI
    // 或系统字体替换后上下裁切；纯文本首列取消内边距，与“路径”标题左对齐。
    _diskTree->setItemHeight(std::max(
        28, QFontMetrics(secondaryFont).height() + 8));
    _diskTree->setItemLeftPadding(0);
    _diskTree->header()->setStretchLastSection(false);
    _diskTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _diskTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rootLayout->addWidget(_diskTree, 1);

    // 空状态提示挂在树的视口上并居中，颜色由 ElaText 自己跟随主题。
    _diskTreeHint = new ElaText(_diskTree->viewport());
    _diskTreeHint->setTextStyle(ElaTextType::Body);
    _diskTreeHint->setTextPixelSize(SecondaryFontPixelSize);
    _diskTreeHint->setAlignment(Qt::AlignCenter);
    _diskTreeHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    _diskTreeHint->hide();
    auto* diskHintLayout = new QVBoxLayout(_diskTree->viewport());
    diskHintLayout->setContentsMargins(12, 12, 12, 12);
    diskHintLayout->addWidget(_diskTreeHint, 0, Qt::AlignCenter);

    connect(_interfaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateNetworkView(); });
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode) { applyTheme(); });

    _fastTimer = new QTimer(this);
    const int configuredInterval = ConfigManager::get<int>(
        QStringLiteral("monitor.fastIntervalMs"), DefaultFastIntervalMs);
    _fastTimer->setInterval(configuredInterval == 1'000
                                ? 1'000 : DefaultFastIntervalMs);
    _fastTimer->setTimerType(Qt::CoarseTimer);
    connect(_fastTimer, &QTimer::timeout,
            this, &SystemMonitorPanel::requestFastMetrics);

    _fileSystemTimer = new QTimer(this);
    _fileSystemTimer->setInterval(FileSystemIntervalMs);
    _fileSystemTimer->setTimerType(Qt::VeryCoarseTimer);
    connect(_fileSystemTimer, &QTimer::timeout,
            this, &SystemMonitorPanel::requestFileSystems);

    retranslateUi();
    applyTheme();
    refreshAvailability();
}

SystemMonitorPanel::~SystemMonitorPanel()
{
    if (!_sshTransport)
        return;
    _sshTransport->stopResourceMonitoring();
    if (_pendingFileSystemRequestId != 0)
        _sshTransport->cancelCommand(_pendingFileSystemRequestId);
}

void SystemMonitorPanel::setSessionContext(const QString& sessionLabel,
                                           SshTransport* transport)
{
    if (transport && _sshTransport == transport && _sessionName == sessionLabel)
        return;

    if (_sshTransport) {
        _sshTransport->stopResourceMonitoring();
        if (_pendingFileSystemRequestId != 0)
            _sshTransport->cancelCommand(_pendingFileSystemRequestId);
        disconnect(_sshTransport, nullptr, this, nullptr);
    }
    _fastTimer->stop();
    _fileSystemTimer->stop();
    _samplingActive = false;
    _sessionName = sessionLabel;
    _sshTransport = transport;
    resetMetrics();

    if (_sshTransport) {
        // 捕获当前 transport，并在回调中复核，屏蔽切换会话后迟到的异步结果。
        SshTransport* const current = _sshTransport.data();
        connect(current, &SshTransport::commandFinished, this,
                [this, current](quint64 requestId, const QByteArray& output,
                                const QByteArray& errorOutput,
                                const QString& errorMessage) {
            if (_sshTransport == current) {
                handleFileSystems(requestId, output, errorOutput,
                                  errorMessage);
            }
        });
        connect(current, &SshTransport::resourceSampleFinished, this,
                [this, current](quint64 requestId, const QByteArray& payload,
                                const QString& errorMessage) {
            if (_sshTransport == current)
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
    }
    updateSamplingState();
    refreshAvailability();
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
    const QString updateDescription = tr(
        "Remote CPU, memory and network update every %1 seconds; "
        "filesystems update every 30 seconds.")
        .arg(_fastTimer->interval() / 1000);
    _infoButton->setToolTip(updateDescription);
    _infoButton->setAccessibleName(updateDescription);
    _cpuLabel->setText(tr("CPU"));
    _memoryLabel->setText(tr("Memory"));
    _swapLabel->setText(tr("Swap"));
    _pathHeader->setText(tr("Path"));
    _capacityHeader->setText(tr("Available / Size"));
    _trafficChart->setAccessibleName(tr("Network traffic history"));
    refreshAvailability();
    updateNetworkView();
}

void SystemMonitorPanel::setDiskTreeHint(const QString& text)
{
    if (!_diskTreeHint)
        return;
    _diskTreeHint->setText(text);
    _diskTreeHint->setVisible(!text.isEmpty());
}

void SystemMonitorPanel::applyTheme()
{    const auto mode = eTheme->getThemeMode();
    _themeMode = mode;

    // 普通文字全部是 ElaText，自己会跟随主题，这里不再逐个设色。只剩两处必须
    // 手工上色：收/发速率标签用的是与流量图两个序列一致的语义色，而 ElaText 的
    // paintEvent 自愈会把任何自定义颜色改回 BasicText，因此它们只能是 QLabel。
    setLabelColor(_receiveLabel, ElaThemeColor(mode, PrimaryNormal));
    setLabelColor(_sendLabel, ElaThemeColor(mode, PrimaryPress));

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
    for (QWidget* widget : QList<QWidget*>{
             _cpuProgress, _memoryProgress, _swapProgress,
             _interfaceCombo, _trafficChart, _diskTree}) {
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

    if (!_hasFileSystems) {
        _diskTree->clear();
        setDiskTreeHint(_fileSystemError.isEmpty()
            ? tr("Waiting for monitoring data") : _fileSystemError);
    }
}

void SystemMonitorPanel::updateSamplingState()
{
    const bool shouldSample = _presentationActive && isVisible()
        && _sshTransport && _sshTransport->isConnected();
    if (_samplingActive == shouldSample)
        return;

    _samplingActive = shouldSample;
    if (!_samplingActive) {
        _fastTimer->stop();
        _fileSystemTimer->stop();
        if (_sshTransport) {
            _sshTransport->stopResourceMonitoring();
            if (_pendingFileSystemRequestId != 0)
                _sshTransport->cancelCommand(_pendingFileSystemRequestId);
        }
        _pendingFastRequestId = 0;
        _pendingFileSystemRequestId = 0;
        // 暂停期间远端累计计数继续变化；恢复时必须重新建立差分基线。
        _previousCpuTotal = 0;
        _previousCpuIdle = 0;
        _previousSampleElapsedMs = -1;
        _previousNetworkBytes.clear();
        _networkRates.clear();
        updateNetworkView();
        return;
    }

    _sampleClock.restart();
    _sshTransport->startResourceMonitoring();
    _fastTimer->start();
    _fileSystemTimer->start();
    QTimer::singleShot(0, this, &SystemMonitorPanel::requestFastMetrics);
    QTimer::singleShot(0, this, &SystemMonitorPanel::requestFileSystems);
}

void SystemMonitorPanel::requestFastMetrics()
{
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
    if (!_samplingActive || !_sshTransport
        || !_sshTransport->isConnected()
        || _pendingFileSystemRequestId != 0) {
        return;
    }
    const quint64 requestId = _nextRequestId++;
    if (_sshTransport->executeCommand(requestId, fileSystemQueryCommand()))
        _pendingFileSystemRequestId = requestId;
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

    const qint64 nowMs = _sampleClock.elapsed();
    // /proc/stat 是开机以来的累计 tick；首个样本仅建立基线，后续才可计算占用率。
    if (_previousCpuTotal > 0 && metrics.cpuTotal > _previousCpuTotal) {
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
}

void SystemMonitorPanel::handleFileSystems(
    quint64 requestId, const QByteArray& standardOutput,
    const QByteArray& standardError, const QString& errorMessage)
{
    if (requestId == 0 || requestId != _pendingFileSystemRequestId)
        return;
    _pendingFileSystemRequestId = 0;

    QList<FileSystemMetric> fileSystems;
    if (!errorMessage.isEmpty()
        || !parseFileSystems(standardOutput, fileSystems)) {
        // 文件系统失败不覆盖快速指标状态；保留最近一次有效缓存。
        if (!_hasFileSystems) {
            const QString details = !errorMessage.isEmpty()
                ? errorMessage : QString::fromUtf8(standardError).trimmed();
            _fileSystemError = details.isEmpty()
                ? tr("No filesystem information available")
                : tr("Filesystem query failed: %1").arg(details);
            setDiskTreeHint(_fileSystemError);
        }
        return;
    }

    _diskTree->clear();
    _fileSystemError.clear();
    setDiskTreeHint({});
    for (const FileSystemMetric& metric : fileSystems) {
        const QString capacity = QStringLiteral("%1 / %2")
            .arg(formatBytes(static_cast<double>(metric.availableKiB) * 1024.0),
                 formatBytes(static_cast<double>(metric.sizeKiB) * 1024.0));
        auto* item = new QTreeWidgetItem(_diskTree, {metric.path, capacity});
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }
    _hasFileSystems = true;
}

void SystemMonitorPanel::updateNetworkView()
{
    const QString interfaceName = _interfaceCombo->currentText();
    const auto rate = _networkRates.constFind(interfaceName);
    if (rate == _networkRates.cend()) {
        _receiveLabel->setText(tr("↑ —"));
        _sendLabel->setText(tr("↓ —"));
    } else {
        _receiveLabel->setText(
            tr("↑ %1/s").arg(formatBytes(rate->first)));
        _sendLabel->setText(
            tr("↓ %1/s").arg(formatBytes(rate->second)));
    }
    _trafficChart->setSamples(_networkHistory.value(interfaceName));
}

void SystemMonitorPanel::resetMetrics()
{
    // transport 上下文变化后累计计数不可跨主机比较，必须连同请求状态一起清空。
    _pendingFastRequestId = 0;
    _pendingFileSystemRequestId = 0;
    _previousCpuTotal = 0;
    _previousCpuIdle = 0;
    _previousSampleElapsedMs = -1;
    _previousNetworkBytes.clear();
    _networkRates.clear();
    _networkHistory.clear();
    _collectionError.clear();
    _fileSystemError.clear();
    _hasMetrics = false;
    _hasFileSystems = false;
    _interfaceCombo->clear();
    _trafficChart->setSamples({});
    for (QProgressBar* bar : {_cpuProgress, _memoryProgress, _swapProgress}) {
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
