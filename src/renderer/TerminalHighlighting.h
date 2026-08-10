/**
 * @file   TerminalHighlighting.h
 * @brief  终端输出高亮规则。
 *
 * 按正则匹配终端输出行，赋予 Error/Warning/Success/Prompt 等高亮角色。
 * 串口会话用它给 ERROR/WARNING 等关键字着色；普通 shell 会话一般不启用。
 */
#pragma once

#include <QRegularExpression>
#include <QString>
#include <QVector>

#include <optional>

namespace NovaTerm {

// 高亮角色：决定匹配行的着色风格。
enum class TerminalHighlightRole
{
    Error,
    Warning,
    Success,
    Prompt
};

// 单条高亮规则：正则 + 角色。
struct TerminalHighlightRule
{
    QRegularExpression pattern;
    TerminalHighlightRole role{TerminalHighlightRole::Warning};
};

// 按规则顺序逐条匹配，首个命中即返回其角色。这种顺序优先级对
// "FAILED, retry OK" 这类行很关键：错误必须先于成功关键字命中，
// 否则会被后续 success 规则掩盖。
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
