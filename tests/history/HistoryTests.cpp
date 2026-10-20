/** @file HistoryTests.cpp
 * @brief 仅经公开接口验证生产历史后端；不用 Qt 或 LZ4 头。
 */
#include "history/ScrollbackBlockCodec.h"
#include "core/scrollback/ChunkedScrollback.h"
#include <iostream>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <stdexcept>
using namespace NovaTerm;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void codecTests() {
    std::vector<LogicalLine> source(3);
    source[0].id = 1; source[1].id = 4; source[2].id = 7;
    source[1].hardBreak = false;
    Cell c; c.chars = {{0x4e2d, 0x1f642, 0x301, 0x302, 0x303, 0x304}}; c.width = 2;
    auto& a = c.attributes;
    a.bold = a.underline = a.italic = a.blink = a.reverse = a.strike = true;
    a.font = a.dwl = a.dhl = a.smallFont = a.baseline = true;
    a.protectedCell = a.dim = a.conceal = true; a.underlineStyle = UnderlineStyle::Curly;
    c.foreground = {ColorType::Indexed, 255, 12, 13, 14};
    c.background = {ColorType::Rgb, 41, 255, 1, 2};
    source[0].cells.push_back(c); c.chars[0] = WideCharContinuation; c.width = 0;
    source[0].cells.push_back(c);
    c = {}; c.chars[0] = ' '; source[2].cells.assign(200, c);
    auto encoded = ScrollbackBlockCodec::encode(source);
    require(bool(encoded), encoded.error.c_str());
    auto decoded = ScrollbackBlockCodec::decode(encoded.block);
    require(bool(decoded), decoded.error.c_str());
    require(decoded.lines.size() == 3 && decoded.lines[1].cells.empty(), "empty row lost");
    require(decoded.lines[1].id == 4 && !decoded.lines[1].hardBreak, "line metadata lost");
    // 显式字段编码相等，保证每个属性/颜色/组合码点相同，不比较 padding。
    auto again = ScrollbackBlockCodec::encode(decoded.lines);
    require(bool(again) && again.block.payload == encoded.block.payload, "semantic fields changed");
    for (unsigned n = 0; n < 8; ++n) {
        auto b = encoded.block;
        switch (n) {
        case 0: ++b.version; break;
        case 1: b.payload.pop_back(); break;
        case 2: ++b.decodedBytes; break;
        case 3: b.index[1].id = b.index[0].id; break;
        case 4: b.index[0].cells = ~u64(0); break;
        case 5: ++b.index[1].offset; break;
        case 6: b.serializedBytes = u64(1) << 40; break;
        default: b.encoding = HistoryEncoding(3); break;
        }
        require(!ScrollbackBlockCodec::decode(b), "invalid input accepted");
    }
    auto b = encoded.block;
    b.payload.resize(1); b.payloadBytes = 1;
    require(!ScrollbackBlockCodec::decode(b), "truncated LZ4 accepted");
    HistoryCodecLimits limits; limits.decodedBytes = 16;
    require(!ScrollbackBlockCodec::decode(encoded.block, limits), "decoded budget ignored");
    source[0].cells[0].foreground.type = ColorType(9);
    require(!ScrollbackBlockCodec::encode(source), "bad enum accepted");
    require(bool(ScrollbackBlockCodec::decode(ScrollbackBlockCodec::encode({}).block)), "empty block failed");
}
void storageTests() {
    ChunkedScrollback storage(-1, 1024 * 1024, 2);
    LogicalLine line; line.cells.resize(10); line.cells[0].chars[0] = 'A';
    storage.append(line);
    const auto active = storage.lineAt(0);
    storage.appendContinuation(line);
    require(active->cells.size() == 10, "active pin mutated");
    auto before = storage.snapshot();
    const auto pin = before.lineAt(0);
    storage.clear();
    require(pin->cells.size() == 20 && before.contains(pin->id), "snapshot pin invalidated");
    require(storage.statistics().retainedBySnapshots > 0, "retained charge missing");
    require(before.lineAt(-1).status() == HistoryReadStatus::NotFound, "not found conflated");
}
void compressionTests() {
    auto runtime = std::make_shared<HistoryCompressionService>();
    std::mutex mutex; std::condition_variable changed; unsigned notifications = 0;
    ChunkedScrollback store(-1, 64 * 1024 * 1024, 64);
    store.setHistoryRuntime(runtime, [&] { std::lock_guard<std::mutex> lock(mutex); ++notifications; changed.notify_all(); });
    HistoryCompressionPolicy policy; policy.enabled = true; policy.hotBytes = 0;
    store.setCompressionPolicy(policy);
    require(store.compressionAvailable(), "parser reservation rejected");
    for (int i = 0; i < 640; ++i) {
        LogicalLine line; line.cells.resize(80);
        for (auto& c : line.cells) c.chars[0] = u32('a' + i % 26);
        store.append(std::move(line));
    }
    auto old = store.snapshot(); const auto raw = store.statistics().effectiveBytes;
    for (int step = 0; step < 30 && store.statistics().compressedBlocks < 9; ++step) {
        unsigned before; { std::lock_guard<std::mutex> lock(mutex); before = notifications; }
        store.processHistory();
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return notifications != before; });
    }
    store.processHistory();
    require(store.statistics().compressedBlocks == 9, "background blocks did not complete");
    require(store.statistics().effectiveBytes < raw / 2, "compression did not save memory");
    require(store.statistics().retainedBySnapshots > 0, "replaced raw snapshot uncharged");
    auto compressed = store.snapshot();
    const auto resourceBefore = runtime->statistics().resources;
    require(compressed.contains(old.firstLineId()) && compressed.lineInfoAt(2).cells == 80, "cold index incorrect");
    require(runtime->statistics().resources.cacheMisses == resourceBefore.cacheMisses, "index query decoded payload");
    for (isize row = 0; row < compressed.lineCount(); ++row) {
        const auto line = compressed.lineAt(row); require(bool(line), "mixed read failed");
        require(line->cells.size() == 80 && line->cells[0].chars[0] == u32('a' + row % 26), "mixed data mismatch");
    }
    const auto pin = compressed.lineAt(0);
    const auto wholeCharge = store.statistics().effectiveBytes;
    store.setLimits(622, 64 * 1024 * 1024);
    auto partial = store.snapshot();
    require(partial.firstLineId() == 19 && partial.lineCount() == 622, "partial compressed eviction lost index");
    require(partial.lineAt(0)->cells[0].chars[0] == u32('a' + 18 % 26), "partial compressed eviction changed data");
    require(store.statistics().effectiveBytes == wholeCharge, "partial payload charge was discounted");
    require(!partial.contains(1) && compressed.contains(1), "partial eviction mutated old snapshot");
    policy.enabled = false; store.setCompressionPolicy(policy);
    LogicalLine tail; require(store.takeNewestTail(80, tail), "raw popline failed");
    // 连续越过热区，Parser 槽不依赖共享缓存可用性。
    for (int i = 0; i < 100; ++i) require(store.takeNewestTail(80, tail), "cold parser popline failed");
    store.clear(); require(store.lineCount() == 0, "clear failed");
    require(pin->cells[0].chars[0] == 'a' && old.lineAt(0)->cells[0].chars[0] == 'a', "legacy handle invalid");
    require(runtime->statistics().resources.decodedBytes <= 32 * 1024 * 1024, "process cache exceeded");
    runtime->stop(); runtime->stop();
    require(runtime->statistics().outstandingTasks == 0, "stop left tasks");
}
void cachePressureTests() {
    HistoryResourceLimits limits; limits.processDecodedBytes = limits.storeDecodedBytes = 128 * 1024;
    HistoryResources resources(limits); auto reader = resources.createContext();
    require(bool(reader), "context allocation failed");
    LogicalLine line; line.id = 1; line.cells.resize(1000);
    auto encoded = ScrollbackBlockCodec::encode({line}); require(bool(encoded), "pressure encode failed");
    auto block = std::make_shared<const EncodedHistoryBlock>(std::move(encoded.block));
    auto pin = reader->read(block, 0, HistoryReadMode::Synchronous); require(bool(pin), "first decode failed");
    resources.trim(); require(pin->cells.size() == 1000, "trim invalidated pin");
    auto other = std::make_shared<const EncodedHistoryBlock>(*block);
    auto busy = reader->read(other, 0, HistoryReadMode::Synchronous);
    require(busy.status() == HistoryReadStatus::Busy, "pinned limit bypassed");
    auto parser = reader->read(other, 0, HistoryReadMode::Parser); require(bool(parser), "reserved parser decode failed");
    pin = {}; resources.trim(); require(resources.statistics().decodedBytes == 0, "cache charge leaked");
}
void multiStoreAndCancellationTests() {
    auto runtime = std::make_shared<HistoryCompressionService>();
    std::vector<std::shared_ptr<HistoryRegistration>> stores;
    std::mutex mutex; std::condition_variable changed; unsigned notices = 0;
    for (int i = 0; i < 8; ++i) stores.push_back(runtime->registerStore([&] { std::lock_guard<std::mutex> lock(mutex); ++notices; changed.notify_all(); }));
    const auto admitted = std::count_if(stores.begin(), stores.end(), [](const auto& item) { return bool(item); });
    require(admitted == 4, "workspace admission did not enforce process cap");
    std::cout << "8 requested stores, " << admitted << " admitted by parser workspace cap\n";
    auto raw = std::make_shared<ScrollbackChunk>(); raw->id = 1;
    for (int i = 0; i < 64; ++i) { LogicalLine line; line.id = u64(i + 1); line.cells.resize(80); raw->lines.push_back(line); }
    raw->sealed = true;
    raw->byteSize = 64 * 80 * isize(sizeof(Cell)) + 8192;
    for (int i = 0; i < 4; ++i) {
        require(runtime->submit(stores[std::size_t(i)], raw), "fair submit rejected first slot");
        require(runtime->submit(stores[std::size_t(i)], raw), "fair submit rejected second slot");
        require(!runtime->submit(stores[std::size_t(i)], raw), "per-store queue limit bypassed");
    }
    runtime->invalidate(stores[0]);
    const auto epoch = stores[0]->epoch(); require(epoch > 1, "clear epoch did not advance");
    std::size_t completed = 0;
    for (int attempt = 0; attempt < 50 && completed < 6; ++attempt) {
        for (int i = 1; i < 4; ++i) { HistoryCompletion result; while (runtime->take(stores[std::size_t(i)], result)) {
            require(bool(result.encoded), "worker did not encode fair task"); ++completed;
        } }
        if (completed < 6) { std::unique_lock<std::mutex> lock(mutex); changed.wait_for(lock, std::chrono::milliseconds(20)); }
    }
    require(completed == 6, "one store starved other stores");
    HistoryCompletion stale; require(!runtime->take(stores[0], stale), "clear resurrected stale completion");
    for (auto& store : stores) runtime->unregisterStore(store);
    stores.clear(); runtime->stop();
    require(runtime->statistics().resources.reservedTemporaryBytes == 4 * 1024 * 1024, "parser slot leaked");
    require(runtime->statistics().outstandingTasks == 0 && runtime->statistics().queuedResultBytes == 0, "mailbox charge leaked");
}
void coldReadDedupAndLifetimeTests() {
    auto runtime = std::make_shared<HistoryCompressionService>();
    std::mutex mutex; std::condition_variable changed; bool notified = false;
    auto registration = runtime->registerStore([&] { std::lock_guard<std::mutex> lock(mutex); notified = true; changed.notify_all(); });
    LogicalLine line; line.id = 1; line.cells.resize(100);
    auto encoded = ScrollbackBlockCodec::encode({line}); require(bool(encoded), "cold encode failed");
    auto block = std::make_shared<const EncodedHistoryBlock>(std::move(encoded.block));
    auto first = registration->reader()->read(block, 0, HistoryReadMode::CacheOnly);
    require(first.status() == HistoryReadStatus::Pending, "cold miss did not request decode");
    for (int i = 0; i < 100; ++i) registration->reader()->read(block, 0, HistoryReadMode::CacheOnly);
    { std::unique_lock<std::mutex> lock(mutex); require(changed.wait_for(lock, std::chrono::seconds(2), [&] { return notified; }), "cold decode did not wake"); }
    auto pin = registration->reader()->read(block, 0, HistoryReadMode::CacheOnly);
    require(bool(pin), "cold decode unavailable");
    require(runtime->statistics().queueHighWater <= 1, "duplicate cold requests queued");
    std::thread reader([pin] { for (int i = 0; i < 1000; ++i) require(pin->cells.size() == 100, "cross-thread pin invalid"); });
    runtime->unregisterStore(registration); runtime->stop(); reader.join();
    require(pin->cells.size() == 100, "stop invalidated pin");
}
void allocationLifetimeTests() {
    auto runtime = std::make_shared<HistoryCompressionService>();
    ScrollbackSnapshot retained;
    {
        ChunkedScrollback storage;
        storage.setHistoryRuntime(runtime);
        LogicalLine line; line.cells.resize(100); storage.append(line);
        retained = storage.snapshot(); storage.processHistory();
        require(runtime->statistics().resources.payloadResidentBytes > 0, "payload charge absent");
    }
    require(runtime->statistics().resources.payloadResidentBytes > 0, "closed owner hid legacy snapshot charge");
    retained = {};
    require(runtime->statistics().resources.payloadResidentBytes == 0, "released snapshot charge leaked");
}
int main() {
    try { codecTests(); storageTests(); compressionTests(); cachePressureTests(); multiStoreAndCancellationTests(); coldReadDedupAndLifetimeTests(); allocationLifetimeTests(); std::cout << "history: codec/storage/cache/service PASS\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
