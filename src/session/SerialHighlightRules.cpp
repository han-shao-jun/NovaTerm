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
