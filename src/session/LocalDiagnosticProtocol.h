/**
 * @file LocalDiagnosticProtocol.h
 * @brief 本机独立诊断 helper 的固定命令目录与输出上限。
 */
#pragma once

#include <QStringView>
#include <QtTypes>

namespace NovaTerm::LocalDiagnostic {

inline constexpr QStringView SystemIdentity{u"system.identity"};
inline constexpr QStringView SystemUptime{u"system.uptime"};
inline constexpr QStringView MemorySummary{u"memory.summary"};
inline constexpr QStringView FilesystemUsage{u"filesystem.usage"};
inline constexpr qsizetype MaxOutputBytes{65536};

/** @brief 检查命令是否属于固定目录；不接受前缀、大小写或额外文本。 */
[[nodiscard]] inline bool isKnownCommand(QStringView command) noexcept
{
    return command == SystemIdentity || command == SystemUptime
        || command == MemorySummary || command == FilesystemUsage;
}

} // namespace NovaTerm::LocalDiagnostic
