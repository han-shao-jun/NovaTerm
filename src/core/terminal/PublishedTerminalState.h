/** @file PublishedTerminalState.h
 * @brief Parser 按需发布的不可变有界终端文本快照。
 */
#pragma once
#include "TerminalState.h"
#include <chrono>

namespace NovaTerm {
struct PublishedTerminalState
{
    TerminalState state;
    std::chrono::system_clock::time_point capturedAt;
};

struct PublishedContextStatistics
{
    u64 requestCount{0};
    u64 publishCount{0};
    u64 reuseCount{0};
};
}
