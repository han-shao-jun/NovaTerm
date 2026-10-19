/** @file HistoryBenchmark.cpp
 * @brief 固定种子与内容哈希的独立进程历史基准；stdout 为 JSON。
 */
#include "core/scrollback/ChunkedScrollback.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
using namespace NovaTerm;
using Clock = std::chrono::steady_clock;
int main(int argc, char** argv) {
    const std::string corpus = argc > 1 ? argv[1] : "variable";
    const int count = argc > 2 ? std::stoi(argv[2]) : 10000;
    const isize budget = argc > 3 ? isize(std::stoll(argv[3])) : ChunkedScrollback::DefaultMaxBytes;
    u64 random = 0x6a09e667f3bcc909ULL, hash = 14695981039346656037ULL;
    const bool enabled = argc > 4 && std::string(argv[4]) == "on";
    const int paceMilliseconds = argc > 5 ? std::max(0, std::stoi(argv[5])) : 0;
    std::mutex mutex; std::condition_variable changed; unsigned notifications = 0;
    auto runtime = std::make_shared<HistoryCompressionService>();
    ChunkedScrollback history(-1, budget);
    history.setHistoryRuntime(runtime, [&] { std::lock_guard<std::mutex> lock(mutex); ++notifications; changed.notify_all(); });
    HistoryCompressionPolicy policy; policy.enabled = enabled;
    history.setCompressionPolicy(policy);
    const auto hashCell = [](u64& value, const Cell& cell) {
        const auto mix = [&](u64 field) { value ^= field; value *= 1099511628211ULL; };
        for (const auto cp : cell.chars) mix(cp);
        mix(cell.width);
        const auto& a = cell.attributes;
        mix(u64(a.bold) | (u64(a.underline) << 1) | (u64(a.italic) << 2) | (u64(a.blink) << 3)
            | (u64(a.reverse) << 4) | (u64(a.strike) << 5) | (u64(a.font) << 6) | (u64(a.dwl) << 7)
            | (u64(a.dhl) << 8) | (u64(a.smallFont) << 9) | (u64(a.baseline) << 10)
            | (u64(a.protectedCell) << 11) | (u64(a.dim) << 12) | (u64(a.conceal) << 13));
        mix(u8(a.underlineStyle));
        for (const auto& c : {cell.foreground, cell.background}) {
            mix(u8(c.type)); mix(c.index); mix(c.red); mix(c.green); mix(c.blue);
        }
    };
    const auto begin = Clock::now();
    u64 cells = 0;
    for (int row = 0; row < count; ++row) {
        LogicalLine line;
        const int width = corpus == "long" ? 30000 : 80;
        line.cells.resize(std::size_t(width));
        const std::string text = "2026-10-17T08:00:00 serial sensor received frame " + std::to_string(row) + " temperature=25.75 OK ";
        for (int col = 0; col < width; ++col) {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            auto& c = line.cells[std::size_t(col)];
            c.chars[0] = corpus == "entropy" ? u32(random % 0x10ffff)
                : corpus == "repeat" ? u32('a' + col % 26) : u32(u8(text[std::size_t(col) % text.size()]));
            if (corpus == "mixed" && col % 5 == 0) {
                c.chars[0] = col % 2 ? 0x4e2d : 0x1f642; c.width = 2;
                c.attributes.bold = true; c.foreground = {ColorType::Rgb, 0, u8(random), 47, 91};
            }
            if (corpus == "entropy") {
                const auto next = [&] { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
                for (auto& cp : c.chars) cp = u32(next());
                c.width = u8(next() % 3); c.attributes.bold = (next() & 1) != 0;
                c.attributes.underlineStyle = UnderlineStyle(next() % 4);
                c.foreground = {ColorType(next() % 3), u8(next()), u8(next()), u8(next()), u8(next())};
                c.background = {ColorType(next() % 3), u8(next()), u8(next()), u8(next()), u8(next())};
            }
            hashCell(hash, c);
        }
        cells += line.cells.size(); history.append(std::move(line));
        if (row % 64 == 0) {
            history.processHistory();
            if (paceMilliseconds) std::this_thread::sleep_for(std::chrono::milliseconds(paceMilliseconds));
        }
    }
    const auto appended = Clock::now();
    history.publish(); history.processHistory();
    for (int round = 0; enabled && round < 10000; ++round) {
        unsigned before; { std::lock_guard<std::mutex> lock(mutex); before = notifications; }
        history.processHistory();
        if (!runtime->statistics().outstandingTasks) break;
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return notifications != before; });
    }
    history.processHistory();
    auto snapshot = history.snapshot();
    u64 verified = 0, retainedHash = 14695981039346656037ULL;
    std::vector<double> readUs;
    const auto scanBegin = Clock::now();
    HistoryReadCursor cursor(snapshot);
    for (isize row = 0; row < snapshot.lineCount(); ++row) {
        const auto start = Clock::now();
        const auto pin = cursor.lineAt(row);
        if (!pin) return 2;
        verified += pin->cells.size();
        for (const auto& cell : pin->cells) hashCell(retainedHash, cell);
        readUs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    }
    const auto end = Clock::now();
    std::sort(readUs.begin(), readUs.end());
    const auto percentile = [&](double p) { return readUs.empty() ? 0.0 : readUs[std::size_t(p * double(readUs.size() - 1))]; };
    std::size_t rss = 0, peak = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) { rss = pmc.WorkingSetSize; peak = pmc.PeakWorkingSetSize; }
#endif
    if (verified == cells && retainedHash != hash) return 3;
    const auto stats = history.statistics();
    const auto service = runtime->statistics();
    const auto resident = stats.effectiveBytes + stats.retainedBySnapshots + stats.decodedBytes + isize(service.resources.reservedTemporaryBytes);
    std::cout << "{\"mode\":\"" << (enabled ? "ON" : "OFF") << "\",\"corpus\":\"" << corpus << "\",\"seed\":\"6a09e667f3bcc909\",\"rows\":" << count
        << ",\"pace_ms_per_64_rows\":" << paceMilliseconds
        << ",\"columns\":" << (corpus == "long" ? 30000 : 80) << ",\"hash_fnv1a64\":\"" << std::hex << hash << std::dec
        << "\",\"retained_hash\":\"" << std::hex << retainedHash << std::dec << "\",\"budget\":" << budget << ",\"retained_lines\":" << stats.logicalLines << ",\"charge\":" << stats.effectiveBytes
        << ",\"resident_charge\":" << resident << ",\"compressed_bytes\":" << stats.compressedBytes
        << ",\"compressed_blocks\":" << stats.compressedBlocks << ",\"compression_failures\":" << stats.compressionFailures
        << ",\"admitted\":" << (history.compressionAvailable() ? 1 : 0)
        << ",\"queue_high_water\":" << service.queueHighWater
        << ",\"rss\":" << rss << ",\"peak_rss\":" << peak << ",\"input_cells\":" << cells << ",\"verified_cells\":" << verified
        << ",\"append_ms\":" << std::chrono::duration<double, std::milli>(appended - begin).count()
        << ",\"settle_ms\":" << std::chrono::duration<double, std::milli>(scanBegin - appended).count()
        << ",\"scan_ms\":" << std::chrono::duration<double, std::milli>(end - scanBegin).count()
        << ",\"read_p95_us\":" << percentile(.95) << ",\"read_p99_us\":" << percentile(.99) << "}\n";
}
