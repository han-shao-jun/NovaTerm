/**
 * @file CommandPlatformProfile.h
 * @brief 受信任命令平台 Profile 的版本与命令能力集合。
 */
#pragma once

#include "SessionTypes.h"

#include <QSet>
#include <QString>

/** @brief 本机代码选择的固定命令平台能力；终端文本不能修改。 */
class CommandPlatformProfile final
{
public:
    CommandPlatformProfile() = default;

    [[nodiscard]] static CommandPlatformProfile sshLinux();
    [[nodiscard]] static CommandPlatformProfile windowsLocal();
    [[nodiscard]] static CommandPlatformProfile forTransport(TransportKind kind);

    [[nodiscard]] bool isAvailable() const noexcept { return !_version.isEmpty(); }
    [[nodiscard]] const QString& version() const noexcept { return _version; }
    [[nodiscard]] const QSet<QString>& supportedCommandIds() const noexcept
    {
        return _supportedCommandIds;
    }
    [[nodiscard]] bool supports(const QString& commandId) const
    {
        return _supportedCommandIds.contains(commandId);
    }

private:
    CommandPlatformProfile(QString version, QSet<QString> supportedCommandIds);

    QString _version;
    QSet<QString> _supportedCommandIds;
};
