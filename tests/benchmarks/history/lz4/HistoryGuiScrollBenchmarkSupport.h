#pragma once

#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace HistoryGuiBenchmark {

inline std::vector<int> jumpTargets(int maximum, int count, int previous)
{
    if (maximum <= 5001 || count < 0 || count > maximum - 5000)
        throw std::invalid_argument("history range too small for distinct jump targets");
    const int span = maximum - 5000;
    int step = 7919;
    if (std::gcd(step, span) != 1) {
        // Keep the usual workload unchanged; for a degenerate modulus use a
        // coprime stride across the same full cold range, not a repeated target.
        step = span / 2 + 1;
        while (std::gcd(step, span) != 1) ++step;
    }
    int start = 173 % span;
    if (1000 + start == previous) start = (start + 1) % span;
    std::vector<int> targets;
    targets.reserve(std::size_t(count));
    for (int i = 0; i < count; ++i)
        targets.push_back(1000 + int((std::int64_t(i) * step + start) % span));
    return targets;
}

} // namespace HistoryGuiBenchmark
