/**
 * @file main.cpp
 * @brief 仅执行固定本机系统 API 查询的独立诊断 helper。
 */
#include "session/LocalDiagnosticProtocol.h"

#include <QCoreApplication>
#include <cstdio>

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
#include <QStorageInfo>
#include <QSysInfo>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <QFile>
#include <time.h>
#endif

namespace {

using namespace NovaTerm::LocalDiagnostic;

static_assert(MaxOutputBytes == 65536);

#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
bool appendLine(QByteArray& output, const QByteArray& line)
{
    if (output.size() >= MaxOutputBytes
        || line.size() > MaxOutputBytes - output.size() - 1) {
        return false;
    }
    output.append(line);
    output.append('\n');
    return true;
}

QByteArray systemIdentity()
{
    QByteArray output;
    static_cast<void>(appendLine(output, "product="
        + QSysInfo::prettyProductName().toUtf8()));
    static_cast<void>(appendLine(output, "kernel="
        + QSysInfo::kernelType().toUtf8() + ' '
        + QSysInfo::kernelVersion().toUtf8()));
    static_cast<void>(appendLine(output, "architecture="
        + QSysInfo::currentCpuArchitecture().toUtf8()));
    return output;
}

QByteArray systemUptime()
{
#ifdef Q_OS_WIN
    return "uptime_seconds=" + QByteArray::number(GetTickCount64() / 1000ULL)
        + '\n';
#else
    timespec uptime{};
    if (clock_gettime(CLOCK_BOOTTIME, &uptime) != 0)
        return {};
    return "uptime_seconds=" + QByteArray::number(uptime.tv_sec) + '\n';
#endif
}

QByteArray memorySummary()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
        return {};
    return "total_bytes=" + QByteArray::number(status.ullTotalPhys) + '\n'
        + "available_bytes=" + QByteArray::number(status.ullAvailPhys) + '\n'
        + "load_percent=" + QByteArray::number(status.dwMemoryLoad) + '\n';
#else
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (!meminfo.open(QIODevice::ReadOnly))
        return {};
    const auto kilobytes = [](const QByteArray& line) -> quint64 {
        const auto fields = line.simplified().split(' ');
        if (fields.size() != 3 || fields.at(2) != "kB")
            return 0;
        bool ok = false;
        const quint64 value = fields.at(1).toULongLong(&ok);
        return ok ? value : 0;
    };
    quint64 totalKiB = 0;
    quint64 availableKiB = 0;
    for (const QByteArray& line : meminfo.read(16 * 1024).split('\n')) {
        if (line.startsWith("MemTotal:")) totalKiB = kilobytes(line);
        else if (line.startsWith("MemAvailable:")) availableKiB = kilobytes(line);
    }
    if (totalKiB == 0 || availableKiB == 0)
        return {};
    return "total_bytes=" + QByteArray::number(totalKiB * 1024) + '\n'
        + "available_bytes=" + QByteArray::number(availableKiB * 1024) + '\n';
#endif
}

QByteArray filesystemUsage()
{
    QByteArray output;
    for (const QStorageInfo& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isValid() || !storage.isReady())
            continue;
        const QByteArray prefix = "root=" + storage.rootPath().toUtf8();
        const QByteArray line = prefix
            + " total_bytes=" + QByteArray::number(storage.bytesTotal())
            + " available_bytes=" + QByteArray::number(storage.bytesAvailable());
        if (!appendLine(output, line))
            break;
    }
    return output;
}
#endif

int reject(const char* message, int exitCode)
{
    std::fprintf(stderr, "%s\n", message);
    return exitCode;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 2
        || !NovaTerm::LocalDiagnostic::isKnownCommand(arguments.at(1))) {
        return reject("Invalid diagnostic request.", 2);
    }

#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
    return reject("Local diagnostics are not enabled on this platform.", 1);
#else
    const QStringView command{arguments.at(1)};
    QByteArray output;
    if (command == SystemIdentity)
        output = systemIdentity();
    else if (command == SystemUptime)
        output = systemUptime();
    else if (command == MemorySummary)
        output = memorySummary();
    else if (command == FilesystemUsage)
        output = filesystemUsage();

    if (output.isEmpty())
        return reject("Diagnostic collection failed.", 1);
    if (output.size() > MaxOutputBytes)
        output.truncate(MaxOutputBytes);
    const size_t written = std::fwrite(output.constData(), 1,
                                       static_cast<size_t>(output.size()), stdout);
    return written == static_cast<size_t>(output.size()) ? 0 : 1;
#endif
}
