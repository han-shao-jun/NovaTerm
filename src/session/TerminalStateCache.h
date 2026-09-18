/** @file TerminalStateCache.h
 *  @brief 有界 revision 日志缓存与重复文本过滤。
 */
#pragma once
#include "core/terminal/TerminalState.h"
#include "TerminalStateFilter.h"
#include <deque>
#include <string_view>
#include <unordered_set>

class TerminalStateCache final
{
public:
    TerminalStateCache() { _seen.reserve(MaxLines + 1); }
    TerminalStateCache(const TerminalStateCache&) = delete;
    TerminalStateCache& operator=(const TerminalStateCache&) = delete;
    struct Entry { NovaTerm::u64 revision; std::string text; };
    void append(NovaTerm::u64 revision, const std::string& text)
    {
        if (!TerminalStateFilter::meaningful(text) || text.size() > MaxBytes)
            return;
        if (_seen.find(text) != _seen.end()) { ++_duplicates; return; }
        _entries.push_back({revision, text});
        // deque 追加不会移动已有元素；索引只借用本缓存自有字符串，不再复制正文。
        _seen.insert(_entries.back().text);
        _bytes += text.size();
        while (_entries.size() > MaxLines || _bytes > MaxBytes) {
            _floor = std::max(_floor, _entries.front().revision);
            _bytes -= _entries.front().text.size();
            _seen.erase(_entries.front().text);
            _entries.pop_front();
        }
    }
    void clear() { _seen.clear(); _entries.clear(); _bytes = 0; _floor = 0; _duplicates = 0; }
    [[nodiscard]] const std::deque<Entry>& entries() const { return _entries; }
    [[nodiscard]] NovaTerm::u64 floor() const { return _floor; }
    [[nodiscard]] NovaTerm::u64 duplicates() const { return _duplicates; }
    static constexpr std::size_t MaxBytes = NovaTerm::TerminalState::MaxBytes;
    static constexpr std::size_t MaxLines = NovaTerm::TerminalState::MaxLines;
private:
    std::deque<Entry> _entries;
    std::unordered_set<std::string_view> _seen;
    std::size_t _bytes{0};
    NovaTerm::u64 _floor{0};
    NovaTerm::u64 _duplicates{0};
};
