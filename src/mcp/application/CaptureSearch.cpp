/** @file CaptureSearch.cpp @brief 统一 capture 搜索实现。 */
#include "CaptureSearch.h"

#include <algorithm>

namespace NovaTerm::Mcp {

CaptureSearchResult searchCapture(const CaptureRecord& capture, const QString& query,
    bool caseSensitive, const QString& scope, int limit,
    const std::function<bool()>& stop)
{
    CaptureSearchResult result;
    const auto fold = [caseSensitive](QByteArray bytes) {
        if (!caseSensitive) {
            for (char& character : bytes) {
                if (character >= 'A' && character <= 'Z')
                    character = char(character + ('a' - 'A'));
            }
        }
        return bytes;
    };
    const auto needle = fold(query.toUtf8());
    if (needle.isEmpty())
        return result;
    int viewportIndex = 0;
    int recentIndex = 0;
    for (const auto& line : capture.lines) {
        const auto& source = line.source;
        const int lineIndex = source == QLatin1String("viewport")
            ? viewportIndex++ : recentIndex++;
        if (scope != QLatin1String("both") && scope != source)
            continue;
        if (stop && stop()) {
            result.interrupted = true;
            return result;
        }
        const auto bytes = fold(line.text.toUtf8());
        qsizetype from = 0;
        while (from <= bytes.size()) {
            if (stop && stop()) {
                result.interrupted = true;
                return result;
            }
            // 每 4 KiB 检查中断；保留 query 长度的重叠，避免漏掉块边界命中。
            const auto span = (std::min)(qsizetype(4096) + needle.size() - 1,
                bytes.size() - from);
            const auto relative = bytes.mid(from, span).indexOf(needle);
            if (relative < 0) {
                if (span < 4096 || from + span == bytes.size())
                    break;
                from += 4096;
                continue;
            }
            const auto found = from + relative;
            if (result.matches.size() == limit) {
                result.limited = true;
                return result;
            }
            result.matches.append(SearchMatch{source, lineIndex, int(found),
                int(found + needle.size())});
            from = found + needle.size();
        }
        if (result.limited)
            return result;
    }
    return result;
}

} // namespace NovaTerm::Mcp
