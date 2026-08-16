/**
 * @file SystemMonitorPanel.cpp
 * @brief 卡片式远端系统资源监视面板与低频 SSH 采集。
 *
 * 复用当前终端的 SSH 连接，通过独立 exec channel 一次性读取 Linux
 * /proc 与 df 数据。轮询间隔为 1 秒，且上一请求完成前不会提交下一请求。
 */
#include "SystemMonitorPanel.h"

#include "ElaComboBox.h"
#include "ElaIconButton.h"
#include "ElaTheme.h"
#include "service/LanguageManager.h"
#include "transport/SshTransport.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QProgressBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

constexpr int NetworkHistoryCapacity = 64;

void setLabelColor(QLabel* label, const QColor& color)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    label->setPalette(palette);
}

QLabel* createLabel(QWidget* parent)
{
    return new QLabel(parent);
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
    QList<FileSystemMetric> fileSystems;
};

QByteArray resourceQueryCommand()
{
    // 单次 exec 批量采集所有指标，避免为每个卡片分别创建远端进程。
    return QByteArrayLiteral(R"NOVATERM(LC_ALL=C; export LC_ALL
awk 'NR == 1 { total = 0; for (i = 2; i <= NF; ++i) total += $i; idle = $5 + $6; printf "CPU\t%.0f\t%.0f\n", total, idle }' /proc/stat 2>/dev/null
awk '/^MemTotal:/ { mt=$2 } /^MemAvailable:/ { ma=$2 } /^MemFree:/ { mf=$2 } /^Buffers:/ { b=$2 } /^Cached:/ { c=$2 } /^SwapTotal:/ { st=$2 } /^SwapFree:/ { sf=$2 } END { if (ma == 0) ma=mf+b+c; printf "MEM\t%.0f\t%.0f\t%.0f\t%.0f\n", mt, ma, st, sf }' /proc/meminfo 2>/dev/null
awk 'NR > 2 { gsub(":", " "); if ($1 != "lo") printf "NET\t%s\t%s\t%s\n", $1, $2, $10 }' /proc/net/dev 2>/dev/null
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
            NetworkMetric metric;
            metric.name = QString::fromUtf8(fields[1]);
            if (parseUnsigned(fields[2], metric.receivedBytes)
                && parseUnsigned(fields[3], metric.sentBytes)) {
                metrics.networks.append(std::move(metric));
            }
        } else if (fields[0] == QByteArrayLiteral("FS")
                   && fields.size() == 4) {
            FileSystemMetric metric;
            metric.path = QString::fromUtf8(fields[1]);
            if (parseUnsigned(fields[2], metric.availableKiB)
                && parseUnsigned(fields[3], metric.sizeKiB)) {
                metrics.fileSystems.append(std::move(metric));
            }
        }
    }
    return hasCpu && hasMemory;
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
        detail->setText(QStringLiteral("0 B / 0 B"));
        return;
    }

    // 先提升到 long double，避免大容量主机上 used * 100 发生整数溢出。
    const int percent = std::clamp(
        static_cast<int>((static_cast<long double>(usedKiB) * 100.0L)
                         / totalKiB),
        0, 100);
    bar->setValue(percent);
    bar->setFormat(QStringLiteral("%1%").arg(percent));
    detail->setText(QStringLiteral("%1 / %2")
        .arg(formatBytes(static_cast<double>(usedKiB) * 1024.0),
             formatBytes(static_cast<double>(totalKiB) * 1024.0)));
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
{
    setMinimumSize(260, 360);
    setAutoFillBackground(false);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(1, 1, 1, 1);
    outerLayout->setSpacing(0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
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

    // 会话名称是资源数据的上下文；信息图标紧邻名称右侧，避免占用独立标题行。
    auto* sessionHeader = new QHBoxLayout;
    sessionHeader->setSpacing(8);
    auto* infoButton = new ElaIconButton(
        ElaIconType::CircleInfo, 14, 28, 28, content);
    infoButton->setFocusPolicy(Qt::NoFocus);
    infoButton->setAttribute(Qt::WA_TransparentForMouseEvents);
    _infoButton = infoButton;
    _sessionLabel = createLabel(content);
    _sessionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sessionHeader->addWidget(_sessionLabel, 0, Qt::AlignVCenter);
    sessionHeader->addWidget(_infoButton, 0, Qt::AlignVCenter);
    sessionHeader->addStretch();
    rootLayout->addLayout(sessionHeader);

    _availabilityLabel = createLabel(content);
    _availabilityLabel->setWordWrap(true);
    rootLayout->addWidget(_availabilityLabel);
    rootLayout->addWidget(createSeparator(content));

    // 移除重复的“服务器资源”标题和未实现的进程入口，资源指标直接展示。
    auto* resourceGrid = new QGridLayout;
    resourceGrid->setHorizontalSpacing(10);
    resourceGrid->setVerticalSpacing(8);
    resourceGrid->setColumnStretch(1, 1);
    _cpuLabel = createLabel(content);
    _memoryLabel = createLabel(content);
    _swapLabel = createLabel(content);
    _cpuProgress = new MetricProgressBar(
        ElaThemeType::PrimaryNormal, content);
    _memoryProgress = new MetricProgressBar(
        ElaThemeType::PrimaryHover, content);
    _swapProgress = new MetricProgressBar(
        ElaThemeType::BasicIndicator, content);
    _cpuDetail = createLabel(content);
    _memoryDetail = createLabel(content);
    _swapDetail = createLabel(content);
    for (QLabel* detail : {_cpuDetail, _memoryDetail, _swapDetail})
        detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    resourceGrid->addWidget(_cpuLabel, 0, 0);
    resourceGrid->addWidget(_cpuProgress, 0, 1);
    resourceGrid->addWidget(_cpuDetail, 0, 2);
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
    _receiveLabel = createLabel(content);
    _sendLabel = createLabel(content);
    _interfaceCombo = new ElaComboBox(content);
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
    _pathHeader = createLabel(content);
    _capacityHeader = createLabel(content);
    _capacityHeader->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fileHeader->addWidget(_pathHeader);
    fileHeader->addStretch();
    fileHeader->addWidget(_capacityHeader);
    rootLayout->addLayout(fileHeader);

    _diskTree = new QTreeWidget(content);
    _diskTree->setColumnCount(2);
    _diskTree->setHeaderHidden(true);
    _diskTree->setRootIsDecorated(false);
    _diskTree->setUniformRowHeights(true);
    _diskTree->setIndentation(0);
    _diskTree->setFrameShape(QFrame::NoFrame);
    _diskTree->setFocusPolicy(Qt::NoFocus);
    _diskTree->setSelectionMode(QAbstractItemView::NoSelection);
    _diskTree->setMinimumHeight(150);
    _diskTree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background: transparent; border: none; }"
        "QTreeWidget::item { padding: 3px 0; }"));
    _diskTree->header()->setStretchLastSection(false);
    _diskTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _diskTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rootLayout->addWidget(_diskTree, 1);

    connect(_interfaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateNetworkView(); });
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode) { applyTheme(); });

    _refreshTimer = new QTimer(this);
    _refreshTimer->setInterval(RefreshIntervalMs);
    // 一秒刷新需要避免 VeryCoarseTimer 的较大抖动，同时无需使用精确定时器增加唤醒开销。
    _refreshTimer->setTimerType(Qt::CoarseTimer);
    connect(_refreshTimer, &QTimer::timeout,
            this, &SystemMonitorPanel::requestMetrics);

    retranslateUi();
    applyTheme();
    refreshAvailability();
}

void SystemMonitorPanel::setSessionContext(const QString& sessionLabel,
                                           SshTransport* transport)
{
    if (transport && _sshTransport == transport && _sessionName == sessionLabel)
        return;

    if (_sshTransport)
        disconnect(_sshTransport, nullptr, this, nullptr);
    _refreshTimer->stop();
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
                handleCommandFinished(requestId, output, errorOutput,
                                      errorMessage);
            }
        });
        connect(current, &ITransport::disconnected, this, [this, current]() {
            if (_sshTransport == current)
                setSessionContext(_sessionName, nullptr);
        });
        connect(current, &QObject::destroyed, this, [this, current]() {
            if (_sshTransport == current)
                setSessionContext(_sessionName, nullptr);
        });
        _refreshTimer->start();
        QTimer::singleShot(0, this, &SystemMonitorPanel::requestMetrics);
    }
    refreshAvailability();
}

void SystemMonitorPanel::retranslateUi()
{
    _infoButton->setToolTip(tr("Remote resources update every second."));
    _infoButton->setAccessibleName(
        tr("Remote resources update every second."));
    _cpuLabel->setText(tr("CPU"));
    _memoryLabel->setText(tr("Memory"));
    _swapLabel->setText(tr("Swap"));
    _pathHeader->setText(tr("Path"));
    _capacityHeader->setText(tr("Available / Size"));
    _trafficChart->setAccessibleName(tr("Network traffic history"));
    refreshAvailability();
    updateNetworkView();
}

void SystemMonitorPanel::applyTheme()
{
    const auto mode = eTheme->getThemeMode();
    const QColor details = ElaThemeColor(mode, BasicDetailsText);
    const QColor category = ElaThemeColor(mode, BasicTextCategory);

    for (QLabel* label : {_sessionLabel, _availabilityLabel,
                          _cpuLabel, _memoryLabel, _swapLabel,
                          _pathHeader, _capacityHeader,
                          _cpuDetail, _memoryDetail, _swapDetail}) {
        setLabelColor(label, details);
    }
    setLabelColor(_receiveLabel, ElaThemeColor(mode, PrimaryNormal));
    setLabelColor(_sendLabel, ElaThemeColor(mode, PrimaryPress));

    QPalette treePalette = _diskTree->palette();
    treePalette.setColor(QPalette::Text, category);
    treePalette.setColor(QPalette::Base, Qt::transparent);
    _diskTree->setPalette(treePalette);

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

    _sessionLabel->setText(_sessionName.isEmpty()
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

    if (_hasMetrics)
        return;

    _diskTree->clear();
    auto* message = new QTreeWidgetItem(
        _diskTree, {tr("Waiting for monitoring data")});
    message->setFlags(message->flags() & ~Qt::ItemIsSelectable);
}

void SystemMonitorPanel::requestMetrics()
{
    // pending ID 同时承担防重入职责：慢服务端未返回时不会继续堆积轮询请求。
    if (!_sshTransport || !_sshTransport->isConnected()
        || _pendingRequestId != 0) {
        return;
    }

    const quint64 requestId = _nextRequestId++;
    if (_sshTransport->executeCommand(requestId, resourceQueryCommand())) {
        _pendingRequestId = requestId;
        if (!_hasMetrics) {
            _collectionError.clear();
            refreshAvailability();
        }
    }
}

void SystemMonitorPanel::handleCommandFinished(
    quint64 requestId, const QByteArray& standardOutput,
    const QByteArray& standardError, const QString& errorMessage)
{
    // 会话切换或重置后 pending ID 会清零，因此旧会话的迟到结果会被忽略。
    if (requestId == 0 || requestId != _pendingRequestId)
        return;
    _pendingRequestId = 0;

    RemoteMetrics metrics;
    if (!errorMessage.isEmpty() || !parseMetrics(standardOutput, metrics)) {
        const QString details = !errorMessage.isEmpty()
            ? errorMessage : QString::fromUtf8(standardError).trimmed();
        _collectionError = details.isEmpty()
            ? tr("The remote system did not return supported Linux metrics.")
            : tr("Remote resource query failed: %1").arg(details);
        refreshAvailability();
        return;
    }
    _collectionError.clear();

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
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
    _cpuDetail->clear();
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
    const double elapsedSeconds = _previousSampleMs > 0
        ? static_cast<double>(nowMs - _previousSampleMs) / 1000.0 : 0.0;
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

    _diskTree->clear();
    // BusyBox 等精简系统不支持 GNU df 的部分选项；远端命令已做兼容回退。
    // 若目标系统仍无法提供文件系统数据，显示明确状态，避免列表区域留白。
    if (metrics.fileSystems.isEmpty()) {
        auto* message = new QTreeWidgetItem(
            _diskTree, {tr("No filesystem information available")});
        message->setFlags(message->flags() & ~Qt::ItemIsSelectable);
    }
    for (const FileSystemMetric& metric : metrics.fileSystems) {
        const QString capacity = QStringLiteral("%1 / %2")
            .arg(formatBytes(static_cast<double>(metric.availableKiB) * 1024.0),
                 formatBytes(static_cast<double>(metric.sizeKiB) * 1024.0));
        auto* item = new QTreeWidgetItem(_diskTree, {metric.path, capacity});
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
    }

    _previousSampleMs = nowMs;
    _hasMetrics = true;
    refreshAvailability();
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
    _pendingRequestId = 0;
    _previousCpuTotal = 0;
    _previousCpuIdle = 0;
    _previousSampleMs = 0;
    _previousNetworkBytes.clear();
    _networkRates.clear();
    _networkHistory.clear();
    _collectionError.clear();
    _hasMetrics = false;
    _interfaceCombo->clear();
    _trafficChart->setSamples({});
    for (QProgressBar* bar : {_cpuProgress, _memoryProgress, _swapProgress}) {
        bar->setValue(0);
        bar->setFormat(QStringLiteral("—"));
    }
    _cpuDetail->clear();
    _memoryDetail->setText(QStringLiteral("—"));
    _swapDetail->setText(QStringLiteral("—"));
    updateNetworkView();
}

void SystemMonitorPanel::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    const auto mode = eTheme->getThemeMode();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(ElaThemeColor(mode, BasicBorder), 1));
    painter.setBrush(ElaThemeColor(mode, DialogBase));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                            8, 8);
}
