/** @file SerialHighlightRules.cpp
 *  Netcatty 默认终端关键词高亮规则，按规则顺序处理重叠匹配。
 */
#include "SerialHighlightRules.h"

namespace NovaTerm {
QVector<TerminalHighlightRule> serialLogHighlightRules()
{
    constexpr auto ci = QRegularExpression::CaseInsensitiveOption;
    const QString ipv4 = QStringLiteral(R"((?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d))");
    const QString ip = QStringLiteral(R"((?<![\w.])(?<!\bver\s)(?<!\bversion\s)(?:%1\.){3}%1(?![\w.]))").arg(ipv4);
    const QString ipv6 = QStringLiteral(R"((?<![\w:.])(?:(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}|(?:[0-9A-Fa-f]{1,4}:){1,7}:|(?:[0-9A-Fa-f]{1,4}:){1,6}:[0-9A-Fa-f]{1,4}|(?:[0-9A-Fa-f]{1,4}:){1,5}(?::[0-9A-Fa-f]{1,4}){1,2}|(?:[0-9A-Fa-f]{1,4}:){1,4}(?::[0-9A-Fa-f]{1,4}){1,3}|(?:[0-9A-Fa-f]{1,4}:){1,3}(?::[0-9A-Fa-f]{1,4}){1,4}|(?:[0-9A-Fa-f]{1,4}:){1,2}(?::[0-9A-Fa-f]{1,4}){1,5}|[0-9A-Fa-f]{1,4}:(?::[0-9A-Fa-f]{1,4}){1,6}|::(?:[0-9A-Fa-f]{1,4}:){0,6}[0-9A-Fa-f]{1,4})(?![\w:.]))");
    return {
        {QRegularExpression(QStringLiteral(R"(\[error\]|\[err\]|\berror\b|\bfail(ed)?\b|\bfatal\b|\bcritical\b|\bexception\b)"), ci), TerminalHighlightRole::Error},
        {QRegularExpression(QStringLiteral(R"(\[warn(ing)?\]|\bwarn(ing)?\b|\bcaution\b|\bdeprecated\b)"), ci), TerminalHighlightRole::Warning},
        {QRegularExpression(QStringLiteral(R"(\[ok\]|\bok\b|\bsuccess(ful)?\b|\bpassed\b|\bcompleted\b|\bdone\b)"), ci), TerminalHighlightRole::Success},
        {QRegularExpression(QStringLiteral(R"(\[info\]|\[notice\]|\[note\]|\bnotice\b|\bnote\b)"), ci), TerminalHighlightRole::Info},
        {QRegularExpression(QStringLiteral(R"(\[debug\]|\[trace\]|\[verbose\]|\bdebug\b|\btrace\b|\bverbose\b)"), ci), TerminalHighlightRole::Debug},
        {QRegularExpression(QStringLiteral(R"REGEX((?:\bhttps?://\[[0-9A-Fa-f:.]+\](?::\d+)?(?:[/?#][^\s<>"'`]*)?(?<![.,;:!?\)}])|\b(?:https?://|www\.)[^\s<>"'`]+(?<![.,;:!?\])}])))REGEX"), ci), TerminalHighlightRole::Network},
        {QRegularExpression(ip, ci), TerminalHighlightRole::Network},
        {QRegularExpression(ipv6, ci), TerminalHighlightRole::Network},
        {QRegularExpression(QStringLiteral(R"(\b([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b)"), ci), TerminalHighlightRole::Network},
        {QRegularExpression(QStringLiteral(R"REGEX((?<=")(?:/|~/)[^"\r\n]+(?=")|(?<=')(?:/|~/)[^'\r\n]+(?=')|(?<![\w.:/"'\x{0080}-\x{FFFF}])(?:/|~/)[^\s"']+)REGEX"), ci), TerminalHighlightRole::Path}
    };
}
} // namespace NovaTerm
