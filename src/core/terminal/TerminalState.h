/**
 * @file TerminalState.h
 * @brief 面向文本消费者的有界解析后状态；不包含 ANSI 或渲染资源。
 */
#pragma once
#include "TerminalTypes.h"
#include <string>
#include <vector>

namespace NovaTerm {
struct TerminalStateLine {
    u64 id{0};
    std::string text;
    bool complete{true};
};
struct TerminalState {
    static constexpr std::size_t MaxBytes = 256 * 1024;
    static constexpr std::size_t MaxLines = 1024;
    u64 revision{0};
    CursorState cursor;
    std::string title;
    bool alternateScreen{false};
    bool truncated{false};
    std::vector<TerminalStateLine> viewport;
    std::vector<TerminalStateLine> recentOutput;
};
} // namespace NovaTerm
