/**
 * @file CommandRiskPolicy.h
 * @brief MCP 自由命令和脚本的保守风险分类接口。
 */
#pragma once

#include <QByteArrayView>
#include <QStringList>
#include <QStringView>

/** @brief 命令静态风险分类。 */
enum class RiskDecision
{
    Allow,
    Confirm,
    Deny,
    Unknown,
};

/** @brief 可供确认界面展示的风险分析结果。 */
struct RiskAssessment
{
    RiskDecision decision{RiskDecision::Unknown};
    QStringList reasons;
    QString policyVersion;
};

/**
 * @brief 尽力检测已知高危行为，并放行明确识别的低风险命令。
 * @note 本策略不是 Shell 沙箱，也不能证明任意脚本安全。
 */
class CommandRiskPolicy final
{
public:
    [[nodiscard]] RiskAssessment classify(QStringView command) const;
    [[nodiscard]] RiskAssessment classifyScript(
        QByteArrayView content, QStringView targetPath,
        QStringView workingDirectory, QStringView invocation) const;
};
