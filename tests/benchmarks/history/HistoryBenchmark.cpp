/** @file HistoryBenchmark.cpp
 * @brief 独立进程历史基准；stdout 只输出一份版本化 JSON。
 */
#include "core/scrollback/ChunkedScrollback.h"
#include "HistoryBenchmarkSupport.h"
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

using namespace NovaTerm;
using namespace HistoryBenchmark;
namespace {
u64 nextRandom(u64& value) {
    value ^= value << 13; value ^= value >> 7; value ^= value << 17;
    return value;
}
int run(int argc, char** argv) {
    require(argc <= 7, "usage: benchmark [corpus rows budget off|on pace_ms read_samples]");
    const std::string corpus = argc > 1 ? argv[1] : "variable";
    require(corpus == "repeat" || corpus == "variable" || corpus == "mixed"
        || corpus == "entropy" || corpus == "long" || corpus == "rewrite", "unknown corpus");
    const int count = argc > 2 ? int(integer(argv[2], 1, 10000000)) : 10000;
    const isize budget = argc > 3 ? isize(integer(argv[3], 1, std::numeric_limits<isize>::max()))
        : ChunkedScrollback::DefaultMaxBytes;
    const std::string mode = argc > 4 ? argv[4] : "off";
    require(mode == "off" || mode == "on", "mode must be off or on");
    const bool enabled = mode == "on";
    const int paceMilliseconds = argc > 5 ? int(integer(argv[5], 0, 1000)) : 0;
    const auto readSamples = argc > 6 ? std::size_t(integer(argv[6], 1, 10000)) : 64;
    u64 random = 0x6a09e667f3bcc909ULL, hash = HashOffset, cells = 0;
    std::vector<u64> expected;
    expected.reserve(std::size_t(count));
    std::mutex mutex;
    std::condition_variable changed;
    u64 notifications = 0;
    auto runtime = std::make_shared<HistoryCompressionService>();
    auto storage = std::make_unique<ChunkedScrollback>(-1, budget);
    auto& history = *storage;
    history.setHistoryRuntime(runtime, [&] {
        std::lock_guard<std::mutex> lock(mutex); ++notifications; changed.notify_all();
    });
    HistoryCompressionPolicy policy; policy.enabled = enabled;
    history.setCompressionPolicy(policy);
    require(!enabled || history.compressionAvailable(), "ON store was not admitted");
    const auto cpuBegin = processCpuMs();
    const auto begin = Clock::now();
    double appendMs = 0;
    std::vector<double> batchUs;
    for (int batch = 0; batch < count; batch += 64) {
        std::vector<LogicalLine> input;
        input.reserve(64);
        std::vector<LogicalLine> suffixes;
        suffixes.reserve(64);
        for (int row = batch; row < std::min(count, batch + 64); ++row) {
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
            }
            line.id = u64(row + 1);
            expected.push_back(lineHash(line));
            mix(hash, expected.back());
            cells += line.cells.size();
            if (corpus == "rewrite") {
                LogicalLine suffix;
                suffix.cells.assign(line.cells.begin() + 40, line.cells.end());
                suffixes.push_back(std::move(suffix));
                line.cells.resize(40); line.hardBreak = false;
            }
            input.push_back(std::move(line));
        }
        // 只计生产存储调用；生成、哈希和节拍均在计时区间外。
        const auto batchBegin = Clock::now();
        for (std::size_t i = 0; i < input.size(); ++i) {
            history.append(std::move(input[i]));
            if (corpus == "rewrite") history.appendContinuation(std::move(suffixes[i]));
        }
        history.processHistory();
        const auto elapsed = milliseconds(Clock::now() - batchBegin);
        appendMs += elapsed; batchUs.push_back(elapsed * 1000.0);
        if (paceMilliseconds) std::this_thread::sleep_for(std::chrono::milliseconds(paceMilliseconds));
    }
    const auto appended = Clock::now();
    const auto publishBegin = Clock::now();
    history.publish(); history.processHistory();
    appendMs += milliseconds(Clock::now() - publishBegin);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        u64 before;
        { std::lock_guard<std::mutex> lock(mutex); before = notifications; }
        history.processHistory();
        if (!runtime->statistics().outstandingTasks) break;
        require(Clock::now() < deadline, "compression settle timed out");
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_until(lock, deadline, [&] { return notifications != before; });
    }
    const auto settled = Clock::now();
    const auto ingestCpu = processCpuMs() - cpuBegin;
    auto snapshot = history.snapshot();
    require(!snapshot.empty(), "budget retained no rows; increase budget");
    const auto beforeRead = runtime->statistics();
    const auto stats = history.statistics();
    const auto retainedMemory = memory();

    // 索引定位单独计时，并用缓存计数证明未触发解码。
    u64 sink = 0;
    const auto indexBegin = Clock::now();
    u64 indexRandom = 0xbb67ae8584caa73bULL;
    for (int i = 0; i < 10000; ++i) {
        const auto row = isize(nextRandom(indexRandom) % u64(snapshot.lineCount()));
        const auto info = snapshot.lineInfoAt(row);
        require(snapshot.contains(info.id) && snapshot.rowForLineId(info.id) == row, "index mismatch");
        sink += info.id + snapshot.firstLineId() + snapshot.lastLineId();
    }
    const double indexMs = milliseconds(Clock::now() - indexBegin);
    const auto afterIndex = runtime->statistics();
    require(beforeRead.resources.cacheMisses == afterIndex.resources.cacheMisses
        && beforeRead.resources.cacheHits == afterIndex.resources.cacheHits, "index decoded history");

    // 顺序扫描只访问字符，不混入昂贵的全字段哈希；校验另走一遍。
    u64 verified = 0, retainedHash = HashOffset;
    std::vector<double> readUs;
    readUs.reserve(std::size_t(snapshot.lineCount()));
    const auto scanCpuBegin = processCpuMs();
    const auto scanBegin = Clock::now();
    {
        HistoryReadCursor cursor(snapshot);
        for (isize row = 0; row < snapshot.lineCount(); ++row) {
            const auto start = Clock::now();
            const auto pin = cursor.lineAt(row);
            readUs.push_back(milliseconds(Clock::now() - start) * 1000.0);
            require(bool(pin), "scan read failed");
            verified += pin->cells.size();
            for (const auto& cell : pin->cells) sink += cell.chars[0];
        }
    }
    const auto scanMs = milliseconds(Clock::now() - scanBegin);
    const auto scanCpu = processCpuMs() - scanCpuBegin;
    // 每个被选块只测首次请求；冷读结束立即测同一行的 CacheOnly 命中。
    std::vector<double> coldUs, coldEncodedUs, hotUs;
    std::size_t encodedSamples = 0;
    const auto& chunks = snapshot.chunks();
    const auto samples = std::min(readSamples, chunks.size());
    for (std::size_t i = 0; i < samples; ++i) {
        const auto& view = chunks[i * chunks.size() / samples];
        const auto row = view.documentStart;
        const auto start = Clock::now();
        auto pin = snapshot.lineAt(row, HistoryReadMode::CacheOnly);
        const bool wasPending = pin.status() == HistoryReadStatus::Pending;
        if (view.chunk->encoded) {
            ++encodedSamples;
            require(wasPending, "cold sample unexpectedly hit cache");
        }
        const auto readDeadline = start + std::chrono::seconds(5);
        while (!pin) {
            require(pin.status() == HistoryReadStatus::Pending, "cold read Busy/Error");
            u64 before;
            { std::lock_guard<std::mutex> lock(mutex); before = notifications; }
            pin = snapshot.lineAt(row, HistoryReadMode::CacheOnly);
            if (pin) break;
            require(Clock::now() < readDeadline, "cold read timed out");
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait_until(lock, readDeadline, [&] { return notifications != before; });
        }
        coldUs.push_back(milliseconds(Clock::now() - start) * 1000.0);
        if (view.chunk->encoded) coldEncodedUs.push_back(coldUs.back());
        require(pin->id > 0 && pin->id <= expected.size()
            && lineHash(*pin) == expected[std::size_t(pin->id - 1)], "cold read content mismatch");
        pin = {};
        const auto hotBegin = Clock::now();
        auto hot = snapshot.lineAt(row, HistoryReadMode::CacheOnly);
        hotUs.push_back(milliseconds(Clock::now() - hotBegin) * 1000.0);
        require(bool(hot), "hot CacheOnly read failed");
        sink += hot->cells.size();
    }

    const auto verifyBegin = Clock::now();
    u64 expectedRetainedHash = HashOffset;
    {
        HistoryReadCursor cursor(snapshot);
        for (isize row = 0; row < snapshot.lineCount(); ++row) {
            const auto pin = cursor.lineAt(row);
            require(bool(pin), "verification read failed");
            const auto info = snapshot.lineInfoAt(row);
            require(pin->id == snapshot.firstLineId() + u64(row) && pin->id == info.id
                && pin->hardBreak == info.hardBreak && pin->cells.size() == info.cells
                && pin->id > 0 && pin->id <= expected.size(), "retained line index mismatch");
            const auto actual = lineHash(*pin);
            require(actual == expected[std::size_t(pin->id - 1)], "retained content mismatch");
            mix(retainedHash, actual);
            mix(expectedRetainedHash, expected[std::size_t(pin->id - 1)]);
        }
    }
    const auto verifyMs = milliseconds(Clock::now() - verifyBegin);
    const auto service = runtime->statistics();
    require(stats.compressionFailures == 0 && stats.storageErrors == 0 && service.failures == 0
        && service.resources.decodeErrors == 0, "history reported errors");
    const auto finalMemory = memory();
    const bool admitted = history.compressionAvailable();
    const auto firstId = snapshot.firstLineId(), lastId = snapshot.lastLineId();
    snapshot = {};
    const auto closeBegin = Clock::now();
    storage.reset();
    const auto closeMs = milliseconds(Clock::now() - closeBegin);
    const auto stopBegin = Clock::now();
    runtime->stop();
    const auto stopMs = milliseconds(Clock::now() - stopBegin);
    const auto closed = runtime->statistics();
    require(closed.resources.payloadResidentBytes == 0 && closed.outstandingTasks == 0,
        "close retained payload/tasks");
    const auto mibPerSecond = [](u64 n, double ms) { return ms > 0 ? double(n) * 1000.0 / ms / 1048576.0 : 0.0; };
#ifdef _WIN32
    const char* platform = "windows";
#elif defined(__APPLE__)
    const char* platform = "macos";
#else
    const char* platform = "unix";
#endif
    std::cout << std::setprecision(10)
        << "{\"schema_version\":2,\"platform\":\"" << platform
        << "\",\"build_type\":\"" << NOVATERM_HISTORY_BENCHMARK_BUILD_TYPE
        << "\",\"compiler\":\"" << NOVATERM_HISTORY_BENCHMARK_COMPILER
        << "\",\"mode\":\"" << (enabled ? "ON" : "OFF") << "\",\"corpus\":\"" << corpus
        << "\",\"seed\":\"6a09e667f3bcc909\",\"rows\":" << count
        << ",\"pace_ms_per_64_rows\":" << paceMilliseconds
        << ",\"columns\":" << (corpus == "long" ? 30000 : 80)
        << ",\"hash_fnv1a64\":\"" << std::hex << hash
        << "\",\"retained_hash\":\"" << retainedHash << "\",\"expected_retained_hash\":\"" << expectedRetainedHash << std::dec
        << "\",\"budget\":" << budget << ",\"retained_lines\":" << stats.logicalLines
        << ",\"first_line_id\":" << firstId << ",\"last_line_id\":" << lastId
        << ",\"evicted_lines\":" << stats.evictedLines << ",\"charge\":" << stats.effectiveBytes
        << ",\"resident_charge\":" << beforeRead.residentHistoryCharge
        << ",\"decoded_bytes_before_read\":" << beforeRead.resources.decodedBytes
        << ",\"decoded_bytes_after_read\":" << service.resources.decodedBytes
        << ",\"compressed_bytes\":" << stats.compressedBytes << ",\"compressed_blocks\":" << stats.compressedBlocks
        << ",\"compression_failures\":" << stats.compressionFailures << ",\"skipped_blocks\":" << stats.skippedBlocks
        << ",\"admitted\":" << (admitted ? 1 : 0)
        << ",\"queue_high_water\":" << service.queueHighWater << ",\"rejected_tasks\":" << service.rejectedTasks
        << ",\"cache_hits\":" << service.resources.cacheHits << ",\"cache_misses\":" << service.resources.cacheMisses
        << ",\"busy_reads\":" << service.resources.busyReads
        << ",\"rss\":" << retainedMemory.rss << ",\"peak_rss\":" << finalMemory.peak
        << ",\"rss_available\":" << (retainedMemory.rssAvailable ? "true" : "false")
        << ",\"input_cells\":" << cells << ",\"verified_cells\":" << verified
        << ",\"append_ms\":" << appendMs << ",\"append_wall_ms\":" << milliseconds(appended - begin)
        << ",\"ingest_cpu_ms\":" << ingestCpu
        << ",\"ingest_wall_ms\":" << milliseconds(settled - begin)
        << ",\"append_cell_mib_per_s\":" << mibPerSecond(cells * sizeof(Cell), appendMs)
        << ",\"batch_samples\":" << batchUs.size()
        << ",\"batch_p95_us\":" << percentile(batchUs, .95) << ",\"batch_p99_us\":" << percentile(batchUs, .99)
        << ",\"settle_ms\":" << milliseconds(settled - appended)
        << ",\"scan_ms\":" << scanMs << ",\"scan_cpu_ms\":" << scanCpu
        << ",\"scan_cell_mib_per_s\":" << mibPerSecond(verified * sizeof(Cell), scanMs)
        << ",\"verify_ms\":" << verifyMs
        << ",\"read_p95_us\":" << percentile(readUs, .95) << ",\"read_p99_us\":" << percentile(readUs, .99)
        << ",\"cold_samples\":" << coldUs.size() << ",\"cold_encoded_samples\":" << encodedSamples
        << ",\"cold_p95_us\":" << percentile(coldUs, .95) << ",\"cold_p99_us\":" << percentile(coldUs, .99)
        << ",\"cold_encoded_p95_us\":" << percentile(coldEncodedUs, .95)
        << ",\"cold_encoded_p99_us\":" << percentile(coldEncodedUs, .99)
        << ",\"hot_p95_us\":" << percentile(hotUs, .95) << ",\"hot_p99_us\":" << percentile(hotUs, .99)
        << ",\"index_operations\":10000,\"index_ms\":" << indexMs
        << ",\"idle_store_close_ms\":" << closeMs << ",\"idle_service_stop_ms\":" << stopMs
        << ",\"checksum_sink\":" << sink << ",\"verified\":true}\n";
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "history benchmark: " << error.what() << '\n'; return 1; }
}
