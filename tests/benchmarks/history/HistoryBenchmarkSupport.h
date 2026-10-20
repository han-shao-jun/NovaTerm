/** @file HistoryBenchmarkSupport.h
 * @brief 历史基准的字段校验、分位数和进程计量；不依赖 Qt。
 */
#pragma once
#include "core/scrollback/ScrollbackData.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#ifdef __linux__
#include <unistd.h>
#endif
#endif

namespace HistoryBenchmark {
using namespace NovaTerm;
using Clock = std::chrono::steady_clock;
inline constexpr u64 HashOffset = 14695981039346656037ULL;
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline void mix(u64& hash, u64 value) { hash ^= value; hash *= 1099511628211ULL; }
inline u64 lineHash(const LogicalLine& line) {
    u64 hash = HashOffset;
    mix(hash, line.id); mix(hash, line.hardBreak); mix(hash, line.cells.size());
    for (const auto& cell : line.cells) {
        for (const auto cp : cell.chars) mix(hash, cp);
        mix(hash, cell.width);
        const auto& a = cell.attributes;
        mix(hash, u64(a.bold) | (u64(a.underline) << 1) | (u64(a.italic) << 2) | (u64(a.blink) << 3)
            | (u64(a.reverse) << 4) | (u64(a.strike) << 5) | (u64(a.font) << 6) | (u64(a.dwl) << 7)
            | (u64(a.dhl) << 8) | (u64(a.smallFont) << 9) | (u64(a.baseline) << 10)
            | (u64(a.protectedCell) << 11) | (u64(a.dim) << 12) | (u64(a.conceal) << 13));
        mix(hash, u8(a.underlineStyle));
        for (const auto& c : {cell.foreground, cell.background}) {
            mix(hash, u8(c.type)); mix(hash, c.index); mix(hash, c.red); mix(hash, c.green); mix(hash, c.blue);
        }
    }
    return hash;
}
inline double milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}
inline double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    // 最近秩定义，P99 至少包含 ceil(0.99*N) 个样本。
    const auto rank = std::size_t(std::ceil(fraction * double(samples.size())));
    return samples[std::max<std::size_t>(1, rank) - 1];
}
inline double processCpuMs() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0, "GetProcessTimes failed");
    const auto ticks = [](FILETIME t) { return (u64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) / 10000.0;
#else
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0
        + double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
#endif
}
struct Memory { u64 rss{0}, peak{0}; bool rssAvailable{false}; };
inline Memory memory() {
    Memory result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof(pmc);
    require(GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) != 0, "GetProcessMemoryInfo failed");
    result = {u64(pmc.WorkingSetSize), u64(pmc.PeakWorkingSetSize), true};
#else
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
#ifdef __APPLE__
    result.peak = u64(usage.ru_maxrss);
#else
    result.peak = u64(usage.ru_maxrss) * 1024;
#endif
#ifdef __linux__
    u64 pages = 0, resident = 0;
    std::ifstream statm("/proc/self/statm");
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if ((statm >> pages >> resident) && pageSize > 0)
        result = {resident * u64(pageSize), result.peak, true};
#endif
#endif
    return result;
}
inline long long integer(const char* text, long long minimum, long long maximum) {
    std::size_t used = 0;
    const auto value = std::stoll(text, &used);
    require(used == std::string(text).size() && value >= minimum && value <= maximum, "argument out of range");
    return value;
}
} // namespace HistoryBenchmark
