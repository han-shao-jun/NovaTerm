/**
 * @file   SerialHighlightRules.cpp
 * @brief  串口日志高亮规则实现。
 *
 * 通过正则匹配串口输出中的错误/警告/成功/提示符关键词，映射到对应高亮角色。
 * 所有规则均大小写不敏感，使用单词边界断言避免误匹配。
 */
#include "SerialHighlightRules.h"

namespace NovaTerm {

QVector<TerminalHighlightRule> serialLogHighlightRules()
{
    using Option = QRegularExpression::PatternOption;
    constexpr Option caseInsensitive =
        QRegularExpression::CaseInsensitiveOption;

    return {
        {QRegularExpression(
             QStringLiteral(
                 R"((?<![A-Za-z0-9])(?:error|failed|failure|fatal|panic|bad|invalid|corrupt)(?![A-Za-z0-9]))"),
             caseInsensitive),
         TerminalHighlightRole::Error},
        {QRegularExpression(
             QStringLiteral(R"((?<![A-Za-z0-9])(?:warn|warning|caution)(?![A-Za-z0-9]))"),
             caseInsensitive),
         TerminalHighlightRole::Warning},
        {QRegularExpression(
             QStringLiteral(R"((?<![A-Za-z0-9])(?:success|successful|passed|ready|done|ok)(?![A-Za-z0-9]))"),
             caseInsensitive),
         TerminalHighlightRole::Success},
        {QRegularExpression(
             QStringLiteral(R"((?:^|\s)\S*[>#$]\s*$)")),
         TerminalHighlightRole::Prompt}
    };
}

} // namespace NovaTerm
