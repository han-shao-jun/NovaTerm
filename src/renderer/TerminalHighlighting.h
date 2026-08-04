#pragma once

#include <QRegularExpression>
#include <QString>
#include <QVector>

#include <optional>

namespace NovaTerm {

enum class TerminalHighlightRole
{
    Error,
    Warning,
    Success,
    Prompt
};

struct TerminalHighlightRule
{
    QRegularExpression pattern;
    TerminalHighlightRole role{TerminalHighlightRole::Warning};
};

// Rules are evaluated in order. This makes precedence explicit for lines such
// as "FAILED, retry OK", where an error must not be hidden by a later success
// token.
inline std::optional<TerminalHighlightRole> matchTerminalHighlight(
    const QVector<TerminalHighlightRule>& rules,
    const QString& text)
{
    for (const TerminalHighlightRule& rule : rules) {
        if (rule.pattern.isValid() && rule.pattern.match(text).hasMatch())
            return rule.role;
    }
    return std::nullopt;
}

} // namespace NovaTerm
