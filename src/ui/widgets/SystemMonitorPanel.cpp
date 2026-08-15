/**
 * @file SystemMonitorPanel.cpp
 * @brief 远端系统资源监视面板布局。
 *
 * 远端采集协议尚未接入，面板保留完整信息层级并使用不可用状态，避免显示
 * 虚构的 CPU、内存、网络或磁盘数据。
 */
#include "SystemMonitorPanel.h"

#include "service/LanguageManager.h"

#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QLabel* createSectionLabel(QWidget* parent)
{
    auto* label = new QLabel(parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QProgressBar* createMetricBar(QWidget* parent)
{
    // 未采集到真实数据时显示破折号，不用 0% 冒充有效监控值。
    auto* bar = new QProgressBar(parent);
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setFormat(QStringLiteral("—"));
    bar->setMinimumHeight(18);
    return bar;
}

} // namespace

SystemMonitorPanel::SystemMonitorPanel(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(240, 220);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(7);

    _sessionLabel = new QLabel(this);
    _sessionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(_sessionLabel);

    _availabilityLabel = new QLabel(this);
    _availabilityLabel->setWordWrap(true);
    rootLayout->addWidget(_availabilityLabel);

    // CPU、内存和交换区属于高频摘要，固定放在面板上半部分。
    _cpuLabel = createSectionLabel(this);
    _cpuProgress = createMetricBar(this);
    _memoryLabel = createSectionLabel(this);
    _memoryProgress = createMetricBar(this);
    _swapLabel = createSectionLabel(this);
    _swapProgress = createMetricBar(this);
    rootLayout->addWidget(_cpuLabel);
    rootLayout->addWidget(_cpuProgress);
    rootLayout->addWidget(_memoryLabel);
    rootLayout->addWidget(_memoryProgress);
    rootLayout->addWidget(_swapLabel);
    rootLayout->addWidget(_swapProgress);

    // 网络和文件系统使用表格承载多条记录，列宽随内容自动调整。
    _networkLabel = createSectionLabel(this);
    rootLayout->addWidget(_networkLabel);
    _networkTree = new QTreeWidget(this);
    _networkTree->setColumnCount(3);
    _networkTree->setRootIsDecorated(false);
    _networkTree->setUniformRowHeights(true);
    _networkTree->header()->setStretchLastSection(false);
    _networkTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _networkTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _networkTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    _networkTree->setMaximumHeight(120);
    rootLayout->addWidget(_networkTree);

    _diskLabel = createSectionLabel(this);
    rootLayout->addWidget(_diskLabel);
    _diskTree = new QTreeWidget(this);
    _diskTree->setColumnCount(2);
    _diskTree->setRootIsDecorated(false);
    _diskTree->setUniformRowHeights(true);
    _diskTree->header()->setStretchLastSection(false);
    _diskTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _diskTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rootLayout->addWidget(_diskTree, 1);

    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) { retranslateUi(); });
    retranslateUi();
    refreshAvailability();
}

void SystemMonitorPanel::setSessionContext(const QString& sessionLabel,
                                           bool sshSessionActive)
{
    _sessionName = sessionLabel;
    _sshSessionActive = sshSessionActive;
    refreshAvailability();
}

void SystemMonitorPanel::retranslateUi()
{
    _cpuLabel->setText(tr("CPU"));
    _memoryLabel->setText(tr("Memory"));
    _swapLabel->setText(tr("Swap"));
    _networkLabel->setText(tr("Network"));
    _diskLabel->setText(tr("File systems"));
    _networkTree->setHeaderLabels(
        {tr("Interface"), tr("Receive"), tr("Send")});
    _diskTree->setHeaderLabels({tr("Path"), tr("Available / Size")});
    refreshAvailability();
}

void SystemMonitorPanel::refreshAvailability()
{
    // 远端指标采集协议尚未接入。后续以真实采集器能力替换此常量，
    // 在此之前保持控件禁用，禁止用本机或虚构数据冒充远端状态。
    constexpr bool backendAvailable = false;
    const bool enabled = _sshSessionActive && backendAvailable;
    _cpuProgress->setEnabled(enabled);
    _memoryProgress->setEnabled(enabled);
    _swapProgress->setEnabled(enabled);
    _networkTree->setEnabled(enabled);
    _diskTree->setEnabled(enabled);

    _sessionLabel->setText(_sessionName.isEmpty()
        ? tr("No active SSH session")
        : tr("Session: %1").arg(_sessionName));
    _availabilityLabel->setText(_sshSessionActive
        ? tr("Remote resource collection is not available yet.")
        : tr("Select a connected SSH terminal to inspect remote resources."));

    // 空状态直接放入对应数据表，后端接入后由网络接口和挂载点条目替代。
    _networkTree->clear();
    auto* networkMessage = new QTreeWidgetItem(
        _networkTree, {tr("Waiting for monitoring data")});
    networkMessage->setFlags(
        networkMessage->flags() & ~Qt::ItemIsSelectable);

    _diskTree->clear();
    auto* diskMessage = new QTreeWidgetItem(
        _diskTree, {tr("Waiting for monitoring data")});
    diskMessage->setFlags(diskMessage->flags() & ~Qt::ItemIsSelectable);
}
