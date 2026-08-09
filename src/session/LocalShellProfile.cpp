#include "LocalShellProfile.h"

#include <QDir>
#include <QFileInfo>

bool LocalShellProfile::isValid() const
{
    return !name.trimmed().isEmpty() && !executable.trimmed().isEmpty();
}

bool LocalShellConfig::isValid() const
{
    return profile.isValid();
}

QString LocalShellConfig::effectiveWorkingDirectory() const
{
    return workingDirectory.isEmpty() ? profile.workingDirectory
                                      : workingDirectory;
}

QProcessEnvironment LocalShellConfig::mergedEnvironment() const
{
    auto merged = QProcessEnvironment::systemEnvironment();
    for (const QString& key : profile.environment.keys())
        merged.insert(key, profile.environment.value(key));
    for (const QString& key : environment.keys())
        merged.insert(key, environment.value(key));
    return merged;
}

namespace {

QString clinkBatchFile(const QString& applicationDirectory)
{
    if (applicationDirectory.isEmpty())
        return {};
    const QFileInfo file(QDir(applicationDirectory).filePath(QStringLiteral("clink.bat")));
    return file.isFile() ? QDir::toNativeSeparators(file.absoluteFilePath()) : QString{};
}

LocalShellProfile profile(QString name, QString executable, QStringList arguments = {})
{
    LocalShellProfile result;
    result.name = std::move(name);
    result.executable = std::move(executable);
    result.arguments = std::move(arguments);
    return result;
}

} // namespace

namespace LocalShellProfiles {

LocalShellProfile commandPrompt(const QString& applicationDirectory)
{
    auto result = profile(QStringLiteral("Command Prompt / Clink"),
                          QStringLiteral("cmd.exe"));
    const QString clink = clinkBatchFile(applicationDirectory);
    if (!clink.isEmpty())
        result.arguments = {QStringLiteral("/k"), clink, QStringLiteral("inject")};
    return result;
}

LocalShellProfile windowsPowerShell()
{
    return profile(QStringLiteral("Windows PowerShell"),
                   QStringLiteral("powershell.exe"));
}

LocalShellProfile powerShell7()
{
    return profile(QStringLiteral("PowerShell 7"), QStringLiteral("pwsh.exe"));
}

LocalShellProfile wsl()
{
    auto result = profile(QStringLiteral("WSL"), QStringLiteral("wsl.exe"));
    result.environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    return result;
}

LocalShellProfile wslDistribution(const QString& distribution)
{
    auto result = wsl();
    result.name = QStringLiteral("WSL (%1)").arg(distribution);
    result.arguments = {QStringLiteral("--distribution"), distribution};
    return result;
}

QList<LocalShellProfile> defaults(const QString& applicationDirectory)
{
    return {commandPrompt(applicationDirectory), windowsPowerShell(), powerShell7(), wsl()};
}

LocalShellProfile platformDefault(const QString& applicationDirectory)
{
#ifdef Q_OS_WIN
    return commandPrompt(applicationDirectory);
#else
    Q_UNUSED(applicationDirectory);
    QString executable = QString::fromLocal8Bit(qgetenv("SHELL"));
    if (executable.isEmpty())
        executable = QStringLiteral("/bin/bash");
    auto result = profile(QStringLiteral("Default Shell"), executable);
    result.environment.insert(QStringLiteral("TERM"),
                              QStringLiteral("xterm-256color"));
    return result;
#endif
}

} // namespace LocalShellProfiles
