/** @file TerminalStateCache.h
 *  @brief 有界 revision 日志缓存与重复文本过滤。
 */
#pragma once
#include "core/terminal/TerminalState.h"
#include "TerminalStateFilter.h"
#include <deque>

class TerminalStateCache final
{
public:
    struct Entry { NovaTerm::u64 revision; std::string text; };
    void append(NovaTerm::u64 revision, const std::string& text)
    {
        if (!TerminalStateFilter::meaningful(text) || text.size() > MaxBytes)
            return;
        for (const auto& entry : _entries) {
            if (entry.text == text) { ++_duplicates; return; }
        }
        _entries.push_back({revision, text});
        _bytes += text.size();
        while (_entries.size() > MaxLines || _bytes > MaxBytes) {
            _floor = std::max(_floor, _entries.front().revision);
            _bytes -= _entries.front().text.size();
            _entries.pop_front();
        }
    }
    void clear() { _entries.clear(); _bytes = 0; _floor = 0; _duplicates = 0; }
    [[nodiscard]] const std::deque<Entry>& entries() const { return _entries; }
    [[nodiscard]] NovaTerm::u64 floor() const { return _floor; }
    [[nodiscard]] NovaTerm::u64 duplicates() const { return _duplicates; }
    static constexpr std::size_t MaxBytes = NovaTerm::TerminalState::MaxBytes;
    static constexpr std::size_t MaxLines = NovaTerm::TerminalState::MaxLines;
private:
    std::deque<Entry> _entries;
    std::size_t _bytes{0};
    NovaTerm::u64 _floor{0};
    NovaTerm::u64 _duplicates{0};
};
