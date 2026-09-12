/**
 * @file LinuxResourceData.h
 * @brief Linux 资源原始数据的本地解析与差分计算。
 */
#pragma once

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <array>
#include <algorithm>

namespace NovaTerm::LinuxResource {

using Sections = QHash<QByteArray, QByteArray>;
using CpuTicks = std::array<quint64, 8>;

inline Sections sections(const QByteArray& raw)
{
    Sections result;
    QByteArray key;
    for (const auto& line : raw.split('\n')) {
        if (line.startsWith("@@"))
            key = line.mid(2).trimmed();
        else if (!key.isEmpty())
            result[key] += line + '\n';
    }
    return result;
}

inline bool cpuTicks(const QByteArray& raw, CpuTicks& ticks)
{
    const auto fields = raw.left(raw.indexOf('\n') < 0 ? raw.size()
                                                     : raw.indexOf('\n'))
                            .simplified().split(' ');
    if (fields.size() < 5 || fields[0] != "cpu")
        return false;
    ticks.fill(0);
    for (qsizetype i = 1; i < std::min<qsizetype>(fields.size(), 9); ++i) {
        bool ok = false;
        ticks[static_cast<size_t>(i - 1)] = fields[i].toULongLong(&ok);
        if (!ok)
            return false;
    }
    return true;
}

inline QByteArray cpuUsage(const CpuTicks& current, const CpuTicks& previous)
{
    CpuTicks delta{};
    quint64 total = 0;
    for (size_t i = 0; i < delta.size(); ++i) {
        // 重启、CPU 热插拔或 iowait 回退时重建基线，不显示异常尖峰。
        if (current[i] < previous[i])
            return {};
        delta[i] = current[i] - previous[i];
        total += delta[i];
    }
    if (total == 0)
        return {};
    QByteArray result("CPUUSE");
    for (auto value : {delta[0], delta[2], delta[1], delta[3], delta[4],
                       delta[5] + delta[6] + delta[7]})
        result += '\t' + QByteArray::number(100.0 * double(value) / double(total), 'f', 1);
    return result + '\n';
}

struct NetworkMetric
{
    QString name;
    quint64 receivedBytes{0};
    quint64 sentBytes{0};
};

struct Sample
{
    CpuTicks ticks{};
    quint64 cpuTotal{0};
    quint64 cpuIdle{0};
    quint64 memoryTotalKiB{0};
    quint64 memoryAvailableKiB{0};
    quint64 cacheKiB{0};
    quint64 swapTotalKiB{0};
    quint64 swapFreeKiB{0};
    QByteArray load;
    QByteArray uptime;
    QList<NetworkMetric> networks;
};

inline bool parseMetrics(const QByteArray& raw, Sample& result)
{
    result = {};
    const auto data = sections(raw);
    if (!cpuTicks(data.value("stat"), result.ticks))
        return false;
    for (auto value : result.ticks)
        result.cpuTotal += value;
    result.cpuIdle = result.ticks[3] + result.ticks[4];
    QHash<QByteArray, quint64> memory;
    for (const auto& line : data.value("meminfo").split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 2)
            continue;
        bool ok = false;
        const auto value = fields[1].toULongLong(&ok);
        if (ok)
            memory.insert(fields[0], value);
    }
    if (!memory.contains("MemTotal:") || memory.value("MemTotal:") == 0)
        return false;
    result.memoryTotalKiB = memory.value("MemTotal:");
    result.cacheKiB = memory.value("Buffers:") + memory.value("Cached:")
        + memory.value("SReclaimable:");
    result.memoryAvailableKiB = std::min(result.memoryTotalKiB,
        memory.contains("MemAvailable:") ? memory.value("MemAvailable:")
        : memory.value("MemFree:") + result.cacheKiB);
    result.swapTotalKiB = memory.value("SwapTotal:");
    result.swapFreeKiB = std::min(result.swapTotalKiB, memory.value("SwapFree:"));
    // meminfo 与 net/dev 可由一次 cat 拼接，减少远端进程创建。
    for (const auto& line : data.value("net/dev", data.value("meminfo")).split('\n')) {
        const auto colon = line.lastIndexOf(':');
        if (colon < 0)
            continue;
        const auto name = line.left(colon).trimmed();
        const auto fields = line.mid(colon + 1).simplified().split(' ');
        if (name == "lo" || fields.size() < 16)
            continue;
        bool rxOk = false, txOk = false;
        const auto rx = fields[0].toULongLong(&rxOk);
        const auto tx = fields[8].toULongLong(&txOk);
        if (rxOk && txOk)
            result.networks.append({QString::fromUtf8(name), rx, tx});
        if (result.networks.size() > 128)
            return false;
    }
    const auto load = data.value("loadavg").simplified().split(' ');
    if (load.size() >= 3)
        result.load = load[0] + ' ' + load[1] + ' ' + load[2];
    result.uptime = QByteArray::number(static_cast<qint64>(
        data.value("uptime").simplified().split(' ').value(0).toDouble()));
    return true;
}

/** @brief 分批查询静态字段；各批间隔由客户端调度，不在远端 sleep。 */
inline QByteArray staticCommand(int batch)
{
    switch (batch) {
    case 0:
        return QByteArrayLiteral(R"NT(LC_ALL=C; export LC_ALL
printf '@@os\n'; cat /etc/os-release 2>/dev/null
IFS= read -r nt_kernel < /proc/sys/kernel/osrelease
IFS= read -r nt_host < /proc/sys/kernel/hostname
printf '\n@@kernel\n%s\n@@host\n%s\n' "$nt_kernel" "$nt_host"
printf '\n@@arch\n'; uname -m 2>/dev/null
printf '\n@@connection\n%s\n' "$SSH_CONNECTION"
printf '\n@@done\n'
)NT");
    case 1:
        return QByteArrayLiteral(R"NT(LC_ALL=C; export LC_ALL
printf '@@cpuinfo\n'; cat /proc/cpuinfo 2>/dev/null
if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]; then
  IFS= read -r nt_freq < /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
  printf '\n@@frequency\n%s\n' "$nt_freq"
fi
printf '\n@@done\n'
)NT");
    case 2:
        return QByteArrayLiteral(R"NT(LC_ALL=C; export LC_ALL
printf '@@ip\n'; ip -o -4 addr show scope global 2>/dev/null
printf '\n@@done\n'
)NT");
    case 3:
        return QByteArrayLiteral(R"NT(LC_ALL=C; export LC_ALL
printf '@@gpu\n'; lspci 2>/dev/null
printf '\n@@done\n'
)NT");
    default:
        return {};
    }
}

/** @brief 频率与容量低频读取；df 独立于快速通道，避免慢挂载阻塞。 */
inline QByteArray slowCommand(bool includeFrequency = true)
{
    QByteArray command = QByteArrayLiteral("LC_ALL=C; export LC_ALL\n");
    if (includeFrequency)
        command += QByteArrayLiteral(R"NT(printf '@@frequency\n'
if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]; then
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null
else
  printf '\n@@cpuinfo\n'; cat /proc/cpuinfo 2>/dev/null
fi
)NT");
    command += QByteArrayLiteral(R"NT(
printf '\n@@filesystems\n'; df -Pk 2>/dev/null || df -k 2>/dev/null
)NT");
    return command;
}

inline QList<QList<QByteArray>> fileSystems(const QByteArray& raw)
{
    QList<QList<QByteArray>> result;
    for (const auto& line : sections(raw).value("filesystems").split('\n')) {
        const auto fields = line.simplified().split(' ');
        bool sizeOk = false, freeOk = false;
        fields.value(1).toULongLong(&sizeOk);
        fields.value(3).toULongLong(&freeOk);
        if (fields.size() < 6 || !sizeOk || !freeOk)
            continue;
        result.append({fields[0], fields[1], fields[4], fields[3],
                       fields.mid(5).join(' ')});
        if (result.size() >= 128)
            break;
    }
    return result;
}

inline QHash<QByteArray, QByteArray> cpuInformation(const QByteArray& raw)
{
    QHash<QByteArray, QByteArray> result;
    int cores = 0;
    for (const auto& line : raw.split('\n')) {
        const auto colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const auto key = line.left(colon).trimmed();
        if (key == "processor")
            ++cores;
        if (!result.contains(key))
            result.insert(key, line.mid(colon + 1).trimmed());
    }
    result.insert("cores", QByteArray::number(cores));
    return result;
}

/** @brief 转为详情窗口已有字段；所有容量与占用计算均在客户端完成。 */
inline QByteArray information(const QByteArray& staticRaw, const QByteArray& slowRaw,
                              const Sample& sample, const QByteArray& usage)
{
    const auto fixed = sections(staticRaw);
    const auto slow = sections(slowRaw);
    const auto cpu = cpuInformation(fixed.value("cpuinfo"));
    QByteArray os;
    for (const auto& line : fixed.value("os").split('\n')) {
        if (line.startsWith("PRETTY_NAME=")) {
            os = line.mid(12).trimmed();
            if (os.size() >= 2 && ((os.front() == '"' && os.back() == '"')
                                  || (os.front() == '\'' && os.back() == '\'')))
                os = os.mid(1, os.size() - 2);
        }
    }
    QByteArray ip;
    for (const auto& line : fixed.value("ip").split('\n')) {
        const auto fields = line.simplified().split(' ');
        const auto index = fields.indexOf("inet");
        if (index >= 0 && index + 1 < fields.size()) {
            ip = fields[index + 1].split('/').value(0);
            break;
        }
    }
    if (ip.isEmpty())
        ip = fixed.value("connection").simplified().split(' ').value(2);
    QByteArray result;
    const auto row = [&result](const QByteArray& type, QList<QByteArray> values) {
        result += type;
        for (auto value : values) {
            value.replace('\t', ' ');
            value.replace('\n', ' ');
            result += '\t' + value.trimmed();
        }
        result += '\n';
    };
    row("OV", {os, fixed.value("kernel"), fixed.value("host"), ip,
               sample.load, fixed.value("arch"), sample.uptime, fixed.value("connection")});
    QByteArray frequency;
    bool frequencyOk = false;
    const double khz = slow.value("frequency", fixed.value("frequency"))
                           .trimmed().toDouble(&frequencyOk);
    if (frequencyOk)
        frequency = QByteArray::number(khz / 1000.0, 'f', 1);
    else
        frequency = cpuInformation(slow.value("cpuinfo", fixed.value("cpuinfo")))
                        .value("cpu MHz");
    if (fixed.contains("cpuinfo"))
        row("CPU", {cpu.value("model name", cpu.value("Processor")), cpu.value("cores"),
                frequency, cpu.value("cache size"),
                cpu.value("vendor_id", cpu.value("Hardware")), cpu.value("BogoMIPS")});
    for (auto line : fixed.value("gpu").split('\n')) {
        if (line.contains("VGA compatible controller") || line.contains("3D controller")
            || line.contains("Display controller"))
            row("GPU", {line.mid(line.indexOf(' ') + 1), "-", "-", "-"});
    }
    result += usage;
    const auto number = [](quint64 value) { return QByteArray::number(value); };
    const auto percent = [](quint64 used, quint64 total) {
        return QByteArray::number(total ? 100.0 * double(used) / double(total) : 0.0, 'f', 1);
    };
    if (sample.memoryTotalKiB > 0) {
        const auto used = sample.memoryTotalKiB - sample.memoryAvailableKiB;
        row("MEM", {number(sample.memoryTotalKiB), number(used), number(sample.memoryAvailableKiB),
                    percent(used, sample.memoryTotalKiB), number(sample.cacheKiB)});
        const auto swapUsed = sample.swapTotalKiB - sample.swapFreeKiB;
        row("SWAP", {number(sample.swapTotalKiB), number(swapUsed), number(sample.swapFreeKiB),
                     percent(swapUsed, sample.swapTotalKiB)});
    }
    for (const auto& network : sample.networks)
        row("NET", {network.name.toUtf8(), number(network.sentBytes), number(network.receivedBytes), "-", "-"});
    for (const auto& fields : fileSystems(slowRaw))
        row("FS", fields);
    return result;
}

} // namespace NovaTerm::LinuxResource
