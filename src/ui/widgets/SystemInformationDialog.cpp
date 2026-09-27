/**
 * @file   SystemInformationDialog.cpp
 * @brief  SSH 远端系统详细信息窗口实现。
 */
#include "SystemInformationDialog.h"

#include "ElaScrollArea.h"
#include "ElaScrollPageArea.h"
#include "ElaText.h"
#include "ElaTheme.h"
#include "service/LanguageManager.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>


namespace {

using Rows = QList<QStringList>;

struct SystemInformation
{
    QStringList overview;
    Rows cpu;
    Rows gpu;
    Rows cpuUsage;
    Rows memory;
    Rows swap;
    Rows networks;
    Rows fileSystems;
};

// unitsPerMiB 指定一个 MiB 对应多少输入单位，避免先乘成字节导致整数溢出。
QString formatCapacity(const QString& value, quint64 unitsPerMiB)
{
    bool ok = false;
    const quint64 amount = value.toULongLong(&ok);
    if (!ok)
        return value.isEmpty() ? QStringLiteral("—") : value;
    // M/G 均表示字节容量，沿用 1024 进位；M 向下取整，G 保留两位小数。
    const quint64 unitsPerGiB = unitsPerMiB * 1024;
    if (amount >= unitsPerGiB)
        return QStringLiteral("%1G")
            .arg(static_cast<double>(amount) / unitsPerGiB, 0, 'f', 2);
    return QStringLiteral("%1M").arg(amount / unitsPerMiB);
}

QString formatKiB(const QString& value)
{
    return formatCapacity(value, 1024);
}

QString formatBytes(const QString& value)
{
    return formatCapacity(value, 1024ULL * 1024ULL);
}

QString dashIfEmpty(const QString& value)
{
    return value.trimmed().isEmpty() ? QStringLiteral("—") : value.trimmed();
}

QString formatDuration(const QString& secondsText)
{
    bool ok = false;
    qint64 seconds = secondsText.toLongLong(&ok);
    if (!ok || seconds < 0)
        return dashIfEmpty(secondsText);
    const qint64 days = seconds / 86'400;
    seconds %= 86'400;
    const qint64 hours = seconds / 3'600;
    const qint64 minutes = (seconds % 3'600) / 60;
    return SystemInformationDialog::tr("%1 d %2 h %3 min")
        .arg(days).arg(hours).arg(minutes);
}

SystemInformation parseInformation(const QByteArray& output)
{
    SystemInformation result;
    for (const QByteArray& rawLine : output.split('\n')) {
        const QList<QByteArray> fields = rawLine.split('\t');
        if (fields.isEmpty())
            continue;
        QStringList values;
        for (qsizetype index = 1; index < fields.size(); ++index)
            values.append(QString::fromUtf8(fields[index]).trimmed());

        if (fields[0] == "OV" && values.size() >= 8) {
            values[6] = formatDuration(values[6]);
            result.overview = values;
        } else if (fields[0] == "CPU" && values.size() >= 6) {
            if (!values[2].isEmpty())
                values[2].append(QStringLiteral(" MHz"));
            // /proc/cpuinfo 的 cache size 以 KB/kB 标记，实际单位为 KiB。
            const QStringList cacheFields = values[3].simplified().split(' ');
            if (cacheFields.size() == 2
                && cacheFields[1].compare(QLatin1String("KB"), Qt::CaseInsensitive) == 0)
                values[3] = formatKiB(cacheFields[0]);
            values[4] = QStringLiteral("%1 / %2")
                .arg(dashIfEmpty(values[4]), dashIfEmpty(values[5]));
            values.removeLast();
            result.cpu.append(values);
        } else if (fields[0] == "GPU" && values.size() >= 4) {
            result.gpu.append(values);
        } else if (fields[0] == "CPUUSE" && values.size() >= 6) {
            for (QString& value : values)
                value.append(QLatin1Char('%'));
            result.cpuUsage.append(values);
        } else if (fields[0] == "MEM" && values.size() >= 5) {
            result.memory.append({formatKiB(values[0]), formatKiB(values[1]),
                                  formatKiB(values[2]), values[3] + '%',
                                  formatKiB(values[4])});
        } else if (fields[0] == "SWAP" && values.size() >= 4) {
            result.swap.append({formatKiB(values[0]), formatKiB(values[1]),
                                formatKiB(values[2]), values[3] + '%'});
        } else if (fields[0] == "NET" && values.size() >= 5) {
            result.networks.append({values[0], formatBytes(values[1]),
                                    formatBytes(values[2]), values[3], values[4]});
        } else if (fields[0] == "FS" && values.size() >= 5) {
            result.fileSystems.append({values[0], formatKiB(values[1]),
                                       values[2], formatKiB(values[3]),
                                       values[4]});
        }
    }
    return result;
}

ElaText* createText(const QString& text, QWidget* parent,
                    bool bold = false, int pixelSize = 13)
{
    auto* label = new ElaText(text, parent);
    label->setTextPixelSize(pixelSize);
    QFont font = label->font();
    font.setBold(bold);
    label->setFont(font);
    label->setWordWrap(true);
    return label;
}

class InformationCard final : public ElaScrollPageArea
{
public:
    explicit InformationCard(const QString& title, QWidget* parent = nullptr)
        : ElaScrollPageArea(parent)
    {
        // ElaScrollPageArea 面向设置页默认固定为 75 高；详情卡片按内容自适应。
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(10);
        layout->addWidget(createText(title, this, true, 14));
        _body = new QVBoxLayout;
        _body->setContentsMargins(0, 0, 0, 0);
        _body->setSpacing(6);
        layout->addLayout(_body);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }

    QVBoxLayout* body() const noexcept { return _body; }

private:
    QVBoxLayout* _body{nullptr};
};

QWidget* createTable(const QStringList& headers, const Rows& rows,
                     QWidget* parent, bool pending)
{
    auto* table = new QWidget(parent);
    auto* grid = new QGridLayout(table);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(8);
    for (qsizetype column = 0; column < headers.size(); ++column) {
        grid->addWidget(createText(headers[column], table, true, 12),
                        0, int(column));
        grid->setColumnStretch(int(column), 1);
    }
    if (rows.isEmpty()) {
        // 详情按批到达：还没拿到的卡片说"采集中"，采完仍为空才是"No data"。
        grid->addWidget(createText(pending
                             ? SystemInformationDialog::tr("Collecting…")
                             : SystemInformationDialog::tr("No data"), table),
                        1, 0, 1, int(headers.size()));
        return table;
    }
    for (qsizetype row = 0; row < rows.size(); ++row) {
        for (qsizetype column = 0; column < headers.size(); ++column) {
            const QString value = column < rows[row].size()
                ? dashIfEmpty(rows[row][column]) : QStringLiteral("—");
            grid->addWidget(createText(value, table),
                            int(row + 1), int(column));
        }
    }
    return table;
}

void clearLayout(QLayout* layout)
{
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            // 先隐藏再延迟删除：面板每秒都会重推数据，deleteLater() 要等回到事件
            // 循环才生效，不隐藏的话旧卡片会与新内容重叠一小段时间。
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

SystemInformationDialog::SystemInformationDialog(
    const QString& sessionName, QWidget* parent)
    : ElaDialog(parent)
    , _sessionName(sessionName)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("System information — %1").arg(_sessionName));
    setWindowButtonFlags(ElaAppBarType::CloseButtonHint);
    setAppBarHeight(32);
    resize(1100, 760);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 32, 0, 0);
    auto* scroll = new ElaScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // ElaScrollArea 默认隐藏两个滚动条；系统信息窗口要求滚动条可见，必须覆盖。
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    root->addWidget(scroll);
    _scroll = scroll;

    auto* content = new QWidget(scroll);
    _contentLayout = new QVBoxLayout(content);
    _contentLayout->setContentsMargins(12, 12, 12, 12);
    _contentLayout->setSpacing(10);
    scroll->setWidget(content);

    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
            this, [this](const QString&) {
        setWindowTitle(tr("System information — %1").arg(_sessionName));
        if (!_lastOutput.isEmpty())
            populate(_lastOutput, _pending);
    });
    connect(eTheme, &ElaTheme::themeModeChanged, this,
            [this](ElaThemeType::ThemeMode) {
        for (QWidget* widget : findChildren<QWidget*>()) {
            if (auto* card = dynamic_cast<InformationCard*>(widget))
                card->update();
        }
        update();
    });
    showStatus(tr("Collecting system information…"));

}

void SystemInformationDialog::showStatus(const QString& text)
{
    clearLayout(_contentLayout);
    auto* status = createText(text, this, false, 14);
    status->setAlignment(Qt::AlignCenter);
    _contentLayout->addWidget(status, 1);
    updateContentHeight();
}

void SystemInformationDialog::populate(const QByteArray& output, bool pending)
{
    _lastOutput = output;
    _pending = pending;
    if (output.isEmpty()) {
        // 首批概览还没到：保持"正在采集"，下一次面板刷新会再进来。
        showStatus(tr("Collecting system information…"));
        return;
    }
    const SystemInformation info = parseInformation(output);
    // 面板每秒都会重推数据；重建期间保留滚动位置，避免用户正在查看时被弹回顶部。
    const int previousScroll = _scroll && _scroll->verticalScrollBar()
        ? _scroll->verticalScrollBar()->value() : 0;
    clearLayout(_contentLayout);

    auto* overview = new InformationCard(tr("Overview"), this);
    auto* overviewGrid = new QGridLayout;
    overviewGrid->setHorizontalSpacing(18);
    overviewGrid->setVerticalSpacing(8);
    const QStringList labels{tr("Operating system"), tr("Kernel version"),
        tr("Host name"), tr("IP"), tr("Load"), tr("Architecture"),
        tr("Uptime"), tr("Connection")};
    for (int index = 0; index < labels.size(); ++index) {
        const int row = index / 2;
        const int pair = index % 2;
        overviewGrid->addWidget(createText(labels[index], overview, true, 12),
                                row, pair * 2);
        overviewGrid->addWidget(createText(
            index < info.overview.size() ? dashIfEmpty(info.overview[index])
                                         : QStringLiteral("—"), overview),
            row, pair * 2 + 1);
        overviewGrid->setColumnStretch(pair * 2 + 1, 1);
    }
    overview->body()->addLayout(overviewGrid);
    _contentLayout->addWidget(overview);

    auto addCard = [this](const QString& title, const QStringList& headers,
                          const Rows& rows) {
        auto* card = new InformationCard(title, this);
        card->body()->addWidget(createTable(headers, rows, card, _pending));
        _contentLayout->addWidget(card);
    };
    addCard(tr("CPU"), {tr("Name"), tr("Cores"), tr("Frequency"),
                         tr("Cache"), tr("Vendor / BogoMIPS")}, info.cpu);
    addCard(tr("GPU"), {tr("Name"), tr("Vendor"), tr("Driver"),
                         tr("Memory")}, info.gpu);
    addCard(tr("CPU usage"), {tr("User"), tr("System"), tr("Nice"),
                               tr("Idle"), tr("IO wait"),
                               tr("IRQ / SoftIRQ / Steal")}, info.cpuUsage);

    auto* memoryRow = new QWidget(this);
    auto* memoryLayout = new QHBoxLayout(memoryRow);
    memoryLayout->setContentsMargins(0, 0, 0, 0);
    memoryLayout->setSpacing(10);
    auto* memory = new InformationCard(tr("Memory"), memoryRow);
    memory->body()->addWidget(createTable(
        {tr("Total"), tr("Used"), tr("Available"), tr("Usage"), tr("Cache")},
        info.memory, memory, _pending));
    auto* swap = new InformationCard(tr("Swap"), memoryRow);
    swap->body()->addWidget(createTable(
        {tr("Total"), tr("Used"), tr("Free"), tr("Usage")}, info.swap, swap,
        _pending));
    memoryLayout->addWidget(memory, 1);
    memoryLayout->addWidget(swap, 1);
    _contentLayout->addWidget(memoryRow);

    addCard(tr("Network interfaces"),
            {tr("Name"), tr("Sent"), tr("Received"),
             tr("Send speed"), tr("Receive speed")}, info.networks);
    addCard(tr("Filesystems"),
            {tr("Name"), tr("Size"), tr("Used"),
             tr("Available"), tr("Mount point")}, info.fileSystems);
    _contentLayout->addStretch();
    updateContentHeight();
    if (previousScroll > 0 && _scroll && _scroll->verticalScrollBar())
        _scroll->verticalScrollBar()->setValue(previousScroll);
}

void SystemInformationDialog::resizeEvent(QResizeEvent* event)
{
    ElaDialog::resizeEvent(event);
    updateContentHeight();
    // 滚动区视口会在随后完成布局，按最终宽度重算，避免首次显示留下旧高度。
    QTimer::singleShot(0, this, &SystemInformationDialog::updateContentHeight);
}

void SystemInformationDialog::updateContentHeight()
{
    if (!_scroll || !_contentLayout)
        return;
    QWidget* const content = _scroll->widget();
    if (!content)
        return;
    _contentLayout->activate();
    const int width = std::max(1, _scroll->viewport()->width());
    // 卡片里的标签开了 word-wrap，布局的 minimumSizeHint() 按极窄宽度估算，
    // 会让滚动区把内容设得远高于实际需要 —— 表现为可以往下滚出大片空白。
    // 用 heightForWidth（按真实视口宽度）算出需要的高度并作为内容最小高度，
    // 滚动范围就与实际内容一致；视口更高时内容仍被拉伸填满，不会出现滚动条。
    int needed = content->heightForWidth(width);
    if (needed <= 0)
        needed = content->sizeHint().height();
    content->setMinimumHeight(needed);
}
