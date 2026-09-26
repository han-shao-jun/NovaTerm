/**
 * @file main.cpp
 * @brief 仅执行固定本机系统 API 查询的独立诊断 helper。
 */
#include "session/LocalDiagnosticProtocol.h"

#include <QCoreApplication>
#include <QStorageInfo>
#include <QSysInfo>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

using namespace NovaTerm::LocalDiagnostic;

static_assert(MaxOutputBytes == 65536);

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

#ifdef Q_OS_WIN
QByteArray systemUptime()
{
    return "uptime_seconds=" + QByteArray::number(GetTickCount64() / 1000ULL)
        + '\n';
}

QByteArray memorySummary()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status))
        return {};
    return "total_bytes=" + QByteArray::number(status.ullTotalPhys) + '\n'
        + "available_bytes=" + QByteArray::number(status.ullAvailPhys) + '\n'
        + "load_percent=" + QByteArray::number(status.dwMemoryLoad) + '\n';
}
#endif

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

#ifndef Q_OS_WIN
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
