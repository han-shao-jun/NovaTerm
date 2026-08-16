/**
 * @file   LocalShellProfile.cpp
 * @brief  本地 shell profile 与预置工厂实现。
 *
 * 提供配置有效性校验、工作目录/环境变量合并逻辑，以及一组预置 profile
 * （命令提示符+Clink、PowerShell、WSL 等）。平台默认值通过 platformDefault()
 * 按编译目标选择。
 */
#include "LocalShellProfile.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QStringConverter>

#include <algorithm>
#include <utility>

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

QString decodeWslListOutput(QByteArray output)
{
    // wsl.exe 在部分 Windows 版本中即使输出被重定向，仍会使用 UTF-16LE；
    // 新版本也可能返回当前代码页文本，因此同时兼容两种编码。
    const bool hasUtf16Bom = output.startsWith("\xFF\xFE");
    const bool looksLikeUtf16 = hasUtf16Bom
        || output.count('\0') > output.size() / 4;
    QString decoded;
    if (looksLikeUtf16) {
        if (hasUtf16Bom)
            output.remove(0, 2);
        if (output.size() % 2 != 0)
            output.chop(1);
        QStringDecoder decoder(QStringDecoder::Utf16LE);
        decoded = decoder.decode(output);
    } else {
        decoded = QString::fromLocal8Bit(output);
    }
    decoded.remove(QChar(0));
    decoded.remove(QChar(0xFEFF));
    return decoded;
}

QStringList parseWslDistributions(const QByteArray& output)
{
    QString text = decodeWslListOutput(output);
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QStringList distributions;
    QSet<QString> seen;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || seen.contains(line))
            continue;
        seen.insert(line);
        distributions.append(std::move(line));
    }
    return distributions;
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

WslDiscoveryResult discoverWslDistributions(int timeoutMs)
{
    WslDiscoveryResult result;
#ifdef Q_OS_WIN
    const QString executable = QStandardPaths::findExecutable(
        QStringLiteral("wsl.exe"));
    if (executable.isEmpty())
        return result;

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--list"), QStringLiteral("--quiet")});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);
    const int boundedTimeout = std::max(1, timeoutMs);
    if (!process.waitForStarted(boundedTimeout))
        return result;
    if (!process.waitForFinished(boundedTimeout)) {
        // 查询超时不能阻塞会话窗口；终止的只是本次列表查询进程。
        process.kill();
        process.waitForFinished(500);
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        return result;
    }

    result.distributions = parseWslDistributions(process.readAllStandardOutput());
    result.status = result.distributions.isEmpty()
        ? WslDiscoveryStatus::NoDistributions
        : WslDiscoveryStatus::Available;
#else
    Q_UNUSED(timeoutMs);
#endif
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
