#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

struct LocalShellProfile
{
    QString name;
    QString executable;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment;

    [[nodiscard]] bool isValid() const;
};

struct LocalShellConfig
{
    LocalShellProfile profile;
    QString workingDirectory;
    QProcessEnvironment environment;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString effectiveWorkingDirectory() const;
    [[nodiscard]] QProcessEnvironment mergedEnvironment() const;
};

namespace LocalShellProfiles {

[[nodiscard]] LocalShellProfile commandPrompt(const QString& applicationDirectory = {});
[[nodiscard]] LocalShellProfile windowsPowerShell();
[[nodiscard]] LocalShellProfile powerShell7();
[[nodiscard]] LocalShellProfile wsl();
[[nodiscard]] LocalShellProfile wslDistribution(const QString& distribution);
[[nodiscard]] QList<LocalShellProfile> defaults(const QString& applicationDirectory = {});
[[nodiscard]] LocalShellProfile platformDefault(const QString& applicationDirectory = {});

} // namespace LocalShellProfiles
