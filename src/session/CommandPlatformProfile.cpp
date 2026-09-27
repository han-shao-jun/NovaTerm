/**
 * @file CommandPlatformProfile.cpp
 * @brief SSH Linux 与 Windows/Linux LocalShell 的固定命令平台 Profile。
 */
#include "CommandPlatformProfile.h"

#include "LocalDiagnosticProtocol.h"

namespace {
QSet<QString> diagnosticCommands()
{
    using namespace NovaTerm::LocalDiagnostic;
    return {SystemIdentity.toString(), SystemUptime.toString(),
            MemorySummary.toString(), FilesystemUsage.toString()};
}
}

CommandPlatformProfile::CommandPlatformProfile(
    QString version, QSet<QString> supportedCommandIds)
    : _version(std::move(version))
    , _supportedCommandIds(std::move(supportedCommandIds))
{
}

CommandPlatformProfile CommandPlatformProfile::sshLinux()
{
    return {QStringLiteral("linux-diagnostics-v2"), diagnosticCommands()};
}

CommandPlatformProfile CommandPlatformProfile::windowsLocal()
{
#ifdef Q_OS_WIN
    return {QStringLiteral("windows-local-v1"), diagnosticCommands()};
#else
    return {};
#endif
}

CommandPlatformProfile CommandPlatformProfile::linuxLocal()
{
#ifdef Q_OS_LINUX
    return {QStringLiteral("linux-local-v1"), diagnosticCommands()};
#else
    return {};
#endif
}

CommandPlatformProfile CommandPlatformProfile::forTransport(TransportKind kind)
{
    switch (kind) {
    case TransportKind::Ssh:
        return sshLinux();
    case TransportKind::LocalShell:
#ifdef Q_OS_WIN
        return windowsLocal();
#else
        return linuxLocal();
#endif
    case TransportKind::Serial:
    case TransportKind::Telnet:
    case TransportKind::Custom:
        return {};
    }
    return {};
}

CommandPlatformProfile CommandPlatformProfile::interactiveFor(
    TransportKind kind)
{
    switch (kind) {
    case TransportKind::Ssh:
        return {QStringLiteral("ssh-interactive-v1"), diagnosticCommands()};
    case TransportKind::LocalShell:
        return {QStringLiteral("local-interactive-v1"), diagnosticCommands()};
    case TransportKind::Serial:
        return {QStringLiteral("serial-interactive-v1"), {}};
    case TransportKind::Telnet:
        return {QStringLiteral("telnet-interactive-v1"), {}};
    case TransportKind::Custom:
        return {};
    }
    return {};
}
