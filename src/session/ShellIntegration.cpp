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

    // Shell/提示符描述对端环境，与承载字节的连接协议无关。
    const QString kind = values.value(QStringLiteral("interactiveShellKind")).toString();
    if (!kind.isEmpty()) {
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
    const QString pattern = values.value(QStringLiteral("interactivePromptPattern")).toString();
    if (!pattern.isEmpty()) {
        if (pattern.size() > 256) return std::nullopt;
        const QRegularExpression expression(pattern);
        if (!expression.isValid())
            return std::nullopt;
        profile.promptPattern = pattern;
        profile.requiresStartMarker = false;
        return profile;
    }
    // 串口等直连设备通常没有可表述的提示符；未配置正则时降级为空闲收尾：
    // 命令 + 行结束符直发，空闲静默判完成。证据弱于提示符/结束标记，
    // 结果按"完成未验证"上报。
    if (runtime.transportKind == TransportKind::Serial) {
        profile.requiresStartMarker = false;
        profile.allowIdleCompletion = true;
        return profile;
    }
    // 保留旧 SSH 未配置时的被动观察兼容行为，不向未知目标自动探测。
    if (runtime.transportKind == TransportKind::Ssh) {
        profile.shellIntegration = true;
        profile.requiresStartMarker = false;
        profile.allowUnverifiedPrompt = true;
        return profile;
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
