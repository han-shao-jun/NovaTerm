/**
 * @file CommandRiskPolicy.cpp
 * @brief 保守命令分类与已知禁止行为扫描。
 */
#include "CommandRiskPolicy.h"

#include <QRegularExpression>
#include <QSet>

#include <utility>

namespace {
const QString RiskPolicyVersion = QStringLiteral("interactive-risk-v1");

RiskAssessment assessment(RiskDecision decision, QString reason)
{
    return {decision, {std::move(reason)}, RiskPolicyVersion};
}

bool hasForbiddenControl(const QString& value)
{
    for (const QChar character : value) {
        if (character.category() == QChar::Other_Control
            && character != QLatin1Char('\n')
            && character != QLatin1Char('\r')
            && character != QLatin1Char('\t')) {
            return true;
        }
    }
    return false;
}

bool containsForbiddenFamily(const QString& command)
{
    static const QRegularExpression forbidden(
        QStringLiteral(R"((?i)(?:^|[\s;&|])(?:sudo|su|runas|doas|pkexec|mkfs(?:\.[a-z0-9]+)?|diskpart|format|shutdown|reboot|poweroff|Get-Credential|Set-ExecutionPolicy)(?=$|[\s;&|])|/etc/shadow|\.ssh[/\\]id_[a-z0-9]+|(?:disable|stop)[\s]+(?:auditd|firewalld|defender))"));
    return forbidden.match(command).hasMatch();
}

bool hasCompoundSyntax(const QString& command)
{
    static const QRegularExpression compound(
        QStringLiteral(R"([\r\n;&|><`]|\$\(|\$\{|\b(?:python|python3|perl|ruby|node|powershell|pwsh|cmd|bash|sh)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return compound.match(command).hasMatch();
}
}

RiskAssessment CommandRiskPolicy::classify(QStringView command) const
{
    const QString text = command.toString().trimmed();
    if (text.isEmpty() || hasForbiddenControl(text))
        return assessment(RiskDecision::Unknown,
                          QStringLiteral("invalid_or_empty_command"));
    if (containsForbiddenFamily(text))
        return assessment(RiskDecision::Deny,
                          QStringLiteral("forbidden_command_family"));
    if (hasCompoundSyntax(text))
        return assessment(RiskDecision::Confirm,
                          QStringLiteral("compound_or_dynamic_shell"));

    static const QSet<QString> readOnly{
        QStringLiteral("uname -srm"),
        QStringLiteral("uptime"),
        QStringLiteral("free -k"),
        QStringLiteral("df -Pk"),
        QStringLiteral("pwd"),
        QStringLiteral("whoami"),
    };
    const QString normalized = text.simplified();
    if (readOnly.contains(normalized))
        return assessment(RiskDecision::Allow,
                          QStringLiteral("strict_read_only_template"));
    if (normalized.startsWith(QStringLiteral("rm "))
        || normalized.startsWith(QStringLiteral("del "))
        || normalized.startsWith(QStringLiteral("mv "))
        || normalized.startsWith(QStringLiteral("cp "))
        || normalized.startsWith(QStringLiteral("install "))) {
        return assessment(RiskDecision::Confirm,
                          QStringLiteral("filesystem_change"));
    }
    return assessment(RiskDecision::Unknown,
                      QStringLiteral("unrecognized_command"));
}

RiskAssessment CommandRiskPolicy::classifyScript(
    QByteArrayView content, QStringView targetPath,
    QStringView workingDirectory, QStringView invocation) const
{
    if (content.isEmpty() || targetPath.isEmpty()
        || workingDirectory.isEmpty() || invocation.isEmpty()) {
        return assessment(RiskDecision::Unknown,
                          QStringLiteral("incomplete_script_request"));
    }
    const QByteArray bytes(content.data(), content.size());
    const QString script = QString::fromUtf8(bytes);
    if (script.toUtf8() != bytes)
        return assessment(RiskDecision::Unknown,
                          QStringLiteral("invalid_script_utf8"));
    const QString combined = script + QLatin1Char('\n')
        + targetPath.toString() + QLatin1Char('\n')
        + workingDirectory.toString() + QLatin1Char('\n')
        + invocation.toString();
    if (containsForbiddenFamily(combined))
        return assessment(RiskDecision::Deny,
                          QStringLiteral("forbidden_script_behavior"));
    return assessment(RiskDecision::Confirm,
                      QStringLiteral("script_requires_confirmation"));
}
