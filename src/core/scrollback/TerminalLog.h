#pragma once
#include "core/CoreTypes.h"
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace NovaTerm {
// A position in committed parsed history. Cell/scalar avoid rescanning long lines.
struct LogPosition {
    u64 line{0}, cell{0}, byte{0};
    unsigned scalar{0};
};
inline bool operator==(const LogPosition& a, const LogPosition& b) {
    return a.line == b.line && a.cell == b.cell && a.scalar == b.scalar && a.byte == b.byte;
}
inline bool operator!=(const LogPosition& a, const LogPosition& b) { return !(a == b); }
struct LogCursor { u64 generation{0}; LogPosition position; };
enum class LogStart { Oldest, Tail };
enum class LogStatus { Ok, Gap, Invalidated, Pending, Busy, Unavailable, BudgetTooSmall, InvalidPosition };
struct LogRequest {
    std::optional<LogCursor> cursor;
    LogStart start{LogStart::Oldest};
    std::size_t maxBytes{65536}, maxLines{256};
};
struct LogFragment {
    u64 line{0}, byteOffset{0};
    std::string text;
    bool lineComplete{false}; // end of this immutable history row
    bool hardBreak{true}; // only a final hard-break fragment includes LF
};
struct LogPage {
    LogStatus status{LogStatus::Ok};
    u64 generation{0};
    LogPosition next, earliest, sampledEnd;
    bool hasMore{false}, caughtUp{false};
    std::vector<LogFragment> fragments;
};
} // namespace NovaTerm
