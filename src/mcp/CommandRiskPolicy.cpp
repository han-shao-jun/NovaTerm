/**
 * @file CommandRiskPolicy.cpp
 * @brief 保守命令分类与已知禁止行为扫描。
 */
#include "CommandRiskPolicy.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <utility>

namespace {
const QString RiskPolicyVersion = QStringLiteral("interactive-risk-v4");

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
        QStringLiteral(R"((?i)(?:^|[\s;&|])(?:sudo|su|runas|runuser|doas|pkexec|mkfs(?:\.[a-z0-9]+)?|diskpart|format|fdisk|sfdisk|parted|wipefs|shutdown|reboot|poweroff|cmdkey|vaultcmd|secret-tool|Get-Credential|Get-Secret|Get-StoredCredential|Set-ExecutionPolicy|Set-MpPreference|Add-MpPreference|setcap|capsh|printenv)(?=$|[\s;&|])|(?:^|[\s;&|])security[\s]+find-generic-password\b|(?:^|[\s;&|])env(?=$|[\s;&|])|Get-ChildItem[\s]+Env:|/etc/(?:shadow|gshadow|sudoers)|/proc/(?:self|[0-9]+)/environ|\.ssh[/\\]id_[a-z0-9]+|(?:^|[/\\])(?:\.aws[/\\]credentials|\.git-credentials|\.netrc|\.npmrc|\.pypirc|\.pgpass|\.docker[/\\]config\.json|\.kube[/\\]config)|\.gnupg[/\\]private-keys-v1\.d|(?:disable|stop)[\s]+(?:auditd|firewalld|defender|winDefend|ufw)|ufw[\s]+disable|iptables[\s]+-F\b|nft[\s]+flush[\s]+ruleset|spctl[\s]+--master-disable|launchctl[\s]+unload|dd[\s]+[^\n]*\bof=/dev/|chmod[\s]+[^\n]*[ugo]?\+s)"));
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
        QStringLiteral("cat /etc/os-release"),
        QStringLiteral("cat /proc/version"),
    };
    const QString normalized = text.simplified();
    if (readOnly.contains(normalized))
        return assessment(RiskDecision::Allow,
                          QStringLiteral("strict_read_only_template"));
    static const QRegularExpression lowRiskList(
        QStringLiteral(R"(^ls(?:\s+-[lahtrS1]+)?(?:\s+\S+)?$)"));
    if (lowRiskList.match(normalized).hasMatch()) {
        return assessment(RiskDecision::Allow,
                          QStringLiteral("simple_low_risk_shell"));
    }
    // cd 不在 Allow 列表里：run_command 的执行目录是当前交互 shell 的动态工作目录，
    // 免确认的 cd 会静默改变此后所有相对路径命令的含义，而这条 cd 在终端里只是一行
    // 普通回显、不进执行记录的原因字段。因此它落到 Unknown -> Confirm。
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
    // 直接从 view 解码并以 hasError() 检测非法 UTF-8：免掉 2 MiB 级的
    // QByteArray 深拷贝与 toUtf8() 往返校验两次分配（classifyScript 单轮
    // 瞬态峰值随之从 ~14 MiB 降到 ~10 MiB）。语义与原回编校验等价。
    QStringDecoder decoder(QStringConverter::Utf8,
                           QStringConverter::Flag::Stateless);
    const QString script = decoder.decode(content);
    if (decoder.hasError())
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
