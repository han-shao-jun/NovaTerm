/**
 * @file ShellIntegration.cpp
 * @brief 交互命令 Profile 的显式配置解析。
 */
#include "ShellIntegration.h"

#include <QRegularExpression>

namespace {
QByteArray lineEnding(const QVariantMap& values)
{
    const QString configured = values.value(
        QStringLiteral("interactiveLineEnding")).toString();
    if (configured == QStringLiteral("lf"))
        return QByteArrayLiteral("\n");
    if (configured == QStringLiteral("crlf"))
        return QByteArrayLiteral("\r\n");
    return QByteArrayLiteral("\r");
}
}

std::optional<InteractiveCommandProfile> ShellIntegration::profileFor(
    const RuntimeConfig& runtime)
{
    const auto& values = runtime.transport;
    InteractiveCommandProfile profile;
    profile.lineEnding = lineEnding(values);
    profile.remoteEcho = values.value(
        QStringLiteral("interactiveRemoteEcho"), true).toBool();

    switch (runtime.transportKind) {
    case TransportKind::LocalShell:
    case TransportKind::Ssh: {
        const QString kind = values.value(
            QStringLiteral("interactiveShellKind")).toString();
        if (kind != QStringLiteral("posix")
            && kind != QStringLiteral("powershell")
            && !(kind == QStringLiteral("cmd")
                 && runtime.transport.value(
                        QStringLiteral("interactiveHookReady")).toBool())) {
            return std::nullopt;
        }
        profile.shellIntegration = true;
        profile.requiresStartMarker = false;
        return profile;
    }
    case TransportKind::Serial:
    case TransportKind::Telnet: {
        const QString pattern = values.value(
            QStringLiteral("interactivePromptPattern")).toString();
        if (pattern.isEmpty() || pattern.size() > 256)
            return std::nullopt;
        const QRegularExpression expression(pattern);
        if (!expression.isValid())
            return std::nullopt;
        profile.promptPattern = pattern;
        profile.requiresStartMarker = false;
        return profile;
    }
    case TransportKind::Custom:
        return std::nullopt;
    }
    return std::nullopt;
}

QMap<QString, QString> ShellIntegration::startupEnvironmentFor(
    const RuntimeConfig& runtime)
{
    if (runtime.transportKind != TransportKind::Ssh
        || runtime.transport.value(QStringLiteral("interactiveShellKind"))
               .toString() != QStringLiteral("posix")) {
        return {};
    }
    return {{QStringLiteral("PROMPT_COMMAND"),
             QStringLiteral("__nvterm_exit=$?; "
                            "__nvterm_prompt=$(( ${__nvterm_prompt:-0} + 1 )); "
                            "printf '\\033]633;NT;PROMPT;%s;%s\\007' "
                            "\"$__nvterm_prompt\" \"$__nvterm_exit\"")}};
}
