/** @file ScriptInvocation.cpp @brief 脚本调用命令构造实现。 */
#include "ScriptInvocation.h"

#include <QLatin1Char>
#include <QStringLiteral>

namespace NovaTerm::Mcp {
namespace {
QString shellQuotePosix(QString value)
{
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
} // namespace

std::optional<QString> scriptInvocationCommand(const QString& shellKind,
    const QString& workingDirectory, const QString& invocation)
{
    if (workingDirectory.isEmpty() || invocation.isEmpty()
        || workingDirectory.contains(QChar::Null)
        || workingDirectory.contains(QLatin1Char('\r'))
        || workingDirectory.contains(QLatin1Char('\n')))
        return std::nullopt;
    if (shellKind == QStringLiteral("posix")) {
        return QStringLiteral("cd -- %1 && %2")
            .arg(shellQuotePosix(workingDirectory), invocation);
    }
    if (shellKind == QStringLiteral("powershell")) {
        QString quoted = workingDirectory;
        quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QStringLiteral("Set-Location -LiteralPath '%1' -ErrorAction Stop; %2")
            .arg(quoted, invocation);
    }
    if (shellKind == QStringLiteral("cmd")) {
        if (workingDirectory.contains(QLatin1Char('"'))
            || workingDirectory.contains(QLatin1Char('%'))
            || workingDirectory.contains(QLatin1Char('^')))
            return std::nullopt;
        QString quoted = workingDirectory;
        quoted.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return QStringLiteral("cd /d \"%1\" && %2").arg(quoted, invocation);
    }
    return std::nullopt;
}

} // namespace NovaTerm::Mcp
