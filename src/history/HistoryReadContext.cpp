/** @file HistoryReadContext.cpp
 * @brief 缓存/pin 统一记账；缓存淘汰不使句柄失效。
 */
#include "HistoryReadContext.h"
#include <atomic>
#include <list>
#include <mutex>
#include <algorithm>
namespace NovaTerm {
namespace {
using Charge = std::shared_ptr<std::atomic<std::size_t>>;
struct DecodedEntry {
    Charge process, store;
    std::weak_ptr<const EncodedHistoryBlock> block;
    std::vector<LogicalLine> lines;
    std::size_t bytes{0};
    std::shared_ptr<std::atomic<u64>> released;
    std::unique_lock<std::mutex> parserLock;
    ~DecodedEntry() {
        std::vector<LogicalLine>().swap(lines);
        if (bytes) { process->fetch_sub(bytes); store->fetch_sub(bytes); if (released) ++*released; }
    }
};
}
struct HistoryResources::State {
    explicit State(HistoryResourceLimits value) : limits(value), reserved(value.workerSlotBytes) {}
    HistoryResourceLimits limits;
    mutable std::mutex mutex;
    Charge payload{std::make_shared<std::atomic<std::size_t>>(0)};
    Charge decoded{std::make_shared<std::atomic<std::size_t>>(0)};
    std::size_t reserved{0};
    std::shared_ptr<std::atomic<u64>> released{std::make_shared<std::atomic<u64>>(0)};
    u64 hits{0}, misses{0}, busy{0}, errors{0};
    std::list<std::shared_ptr<DecodedEntry>> lru;
    struct InFlight { std::weak_ptr<const EncodedHistoryBlock> block; Charge store; };
    std::vector<InFlight> loading;
    void trim() {
        for (auto it = lru.begin(); it != lru.end();) {
            if (it->use_count() == 1) it = lru.erase(it); else ++it;
        }
    }
    bool acquire(const Charge& store, std::size_t bytes) {
        if (bytes > limits.processDecodedBytes || bytes > limits.storeDecodedBytes) return false;
        if (decoded->load() + bytes > limits.processDecodedBytes || store->load() + bytes > limits.storeDecodedBytes) trim();
        if (decoded->load() + bytes > limits.processDecodedBytes || store->load() + bytes > limits.storeDecodedBytes) { ++busy; return false; }
        decoded->fetch_add(bytes); store->fetch_add(bytes); return true;
    }
};
struct HistoryReadContext::State {
    explicit State(std::shared_ptr<HistoryResources::State> value) : resources(std::move(value)) {}
    ~State() { std::lock_guard<std::mutex> lock(resources->mutex); resources->reserved -= resources->limits.parserSlotBytes; }
    std::shared_ptr<HistoryResources::State> resources;
    Charge decoded{std::make_shared<std::atomic<std::size_t>>(0)};
    std::mutex parser, callbacks;
    std::function<bool(std::shared_ptr<const EncodedHistoryBlock>)> coldRequest;
    std::function<void()> wake;
    struct ColdRead {
        std::weak_ptr<const EncodedHistoryBlock> block;
        HistoryReadStatus status{HistoryReadStatus::Pending};
        u64 released{0};
        std::string error;
    };
    std::vector<ColdRead> cold;
};
HistoryResources::HistoryResources(HistoryResourceLimits limits) : _state(std::make_shared<State>(limits)) {}
HistoryResources::~HistoryResources() = default;
std::shared_ptr<HistoryReadContext> HistoryResources::createContext() {
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (_state->limits.parserSlotBytes < 3 * 1024 * 1024 || _state->limits.workerSlotBytes < 4 * 1024 * 1024) return {};
        if (_state->reserved > _state->limits.temporaryBytes
            || _state->limits.parserSlotBytes > _state->limits.temporaryBytes - _state->reserved) return {};
        _state->reserved += _state->limits.parserSlotBytes;
    }
    // 构造失败在锁外回收；State 析构会自行取得预算锁。
    std::unique_ptr<HistoryReadContext> context;
    try { context.reset(new HistoryReadContext(_state)); }
    catch (...) { std::lock_guard<std::mutex> lock(_state->mutex); _state->reserved -= _state->limits.parserSlotBytes; throw; }
    return std::shared_ptr<HistoryReadContext>(std::move(context));
}
HistoryResourceStatistics HistoryResources::statistics() const {
    std::lock_guard<std::mutex> lock(_state->mutex);
    HistoryResourceStatistics s; s.decodedBytes = _state->decoded->load(); s.payloadResidentBytes = _state->payload->load(); s.reservedTemporaryBytes = _state->reserved;
    s.pinnedDecodedBytes = s.decodedBytes;
    for (const auto& entry : _state->lru) if (entry.use_count() == 1) s.pinnedDecodedBytes -= entry->bytes;
    s.cacheHits = _state->hits; s.cacheMisses = _state->misses; s.busyReads = _state->busy; s.decodeErrors = _state->errors;
    return s;
}
std::shared_ptr<HistoryAllocationCharge> HistoryResources::trackPayload(std::size_t bytes) {
    auto charge = std::make_shared<HistoryAllocationCharge>(_state->payload); charge->set(bytes); return charge;
}
void HistoryResources::trim() { std::lock_guard<std::mutex> lock(_state->mutex); _state->trim(); }
HistoryReadContext::HistoryReadContext(std::shared_ptr<HistoryResources::State> resources) : _state(std::make_shared<State>(std::move(resources))) {}
HistoryReadContext::~HistoryReadContext() = default;
HistoryResourceStatistics HistoryReadContext::statistics() const {
    const auto& r = _state->resources; std::lock_guard<std::mutex> lock(r->mutex);
    HistoryResourceStatistics s; s.decodedBytes = _state->decoded->load();
    s.pinnedDecodedBytes = s.decodedBytes;
    for (const auto& entry : r->lru) if (entry->store == _state->decoded && entry.use_count() == 1) s.pinnedDecodedBytes -= entry->bytes;
    s.reservedTemporaryBytes = r->limits.parserSlotBytes; return s;
}
PinnedHistoryLine HistoryReadContext::read(std::shared_ptr<const EncodedHistoryBlock> block, isize row, HistoryReadMode mode) {
    if (!block || row < 0 || std::size_t(row) >= block->index.size()) return {};
    if (block->decodedBytes > 1024 * 1024 || block->serializedBytes > 1024 * 1024)
        return PinnedHistoryLine(HistoryReadStatus::DecodeError, "history size limit");
    const auto r = _state->resources;
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        for (auto it = r->lru.begin(); it != r->lru.end(); ++it) {
            if ((*it)->block.lock() == block && (*it)->store == _state->decoded) {
                auto entry = *it; r->lru.splice(r->lru.end(), r->lru, it); ++r->hits;
                return {entry, &entry->lines[std::size_t(row)]};
            }
        }
        ++r->misses;
    }
    if (mode == HistoryReadMode::CacheOnly) {
        std::lock_guard<std::mutex> lock(_state->callbacks);
        _state->cold.erase(std::remove_if(_state->cold.begin(), _state->cold.end(), [](const auto& item) { return item.block.expired(); }), _state->cold.end());
        auto found = std::find_if(_state->cold.begin(), _state->cold.end(), [&](const auto& item) { return item.block.lock() == block; });
        if (found != _state->cold.end()) {
            if (found->status != HistoryReadStatus::Busy || found->released == r->released->load())
                return PinnedHistoryLine(found->status, found->error);
            _state->cold.erase(found);
        }
        if (_state->cold.size() >= 32) return PinnedHistoryLine(HistoryReadStatus::Busy);
        _state->cold.push_back({block, HistoryReadStatus::Pending, 0, {}});
        const bool queued = _state->coldRequest && _state->coldRequest(block);
        if (!queued) _state->cold.pop_back();
        return PinnedHistoryLine(queued ? HistoryReadStatus::Pending : HistoryReadStatus::Busy);
    }
    // Parser 使用预留槽，绝不等待 worker 或共享缓存额度。
    std::shared_ptr<void> loadingToken;
    auto entry = std::make_shared<DecodedEntry>();
    if (mode == HistoryReadMode::Parser) {
        entry->parserLock = std::unique_lock<std::mutex>(_state->parser, std::try_to_lock);
        if (!entry->parserLock.owns_lock()) return PinnedHistoryLine(HistoryReadStatus::Busy);
    } else {
        // 同步 worker 的序列化缓冲也计入解码额度；同块并发最终合并进 LRU。
        const auto charge = std::size_t(block->decodedBytes + block->serializedBytes) + sizeof(DecodedEntry) + 256;
        std::lock_guard<std::mutex> lock(r->mutex);
        for (const auto& item : r->loading) if (item.block.lock() == block && item.store == _state->decoded)
            return PinnedHistoryLine(HistoryReadStatus::Busy);
        if (!r->acquire(_state->decoded, charge)) return PinnedHistoryLine(HistoryReadStatus::Busy);
        entry->process = r->decoded; entry->store = _state->decoded; entry->bytes = charge; entry->released = r->released;
        const auto store = _state->decoded;
        r->loading.push_back({block, store});
        loadingToken = std::shared_ptr<void>(reinterpret_cast<void*>(1), [r, block, store](void*) {
            std::lock_guard<std::mutex> guard(r->mutex);
            r->loading.erase(std::remove_if(r->loading.begin(), r->loading.end(), [&](const auto& item) {
                return item.block.lock() == block && item.store == store;
            }), r->loading.end());
        });
    }
    auto result = ScrollbackBlockCodec::decode(*block);
    if (!result) {
        std::lock_guard<std::mutex> lock(r->mutex); ++r->errors;
        return PinnedHistoryLine(HistoryReadStatus::DecodeError, result.error);
    }
    entry->lines = std::move(result.lines);
    entry->block = std::move(block);
    // Parser 句柄保持 context，直到读完才允许释放槽与 parser mutex。
    if (mode == HistoryReadMode::Parser) {
        struct ParserPin { std::shared_ptr<State> state; std::shared_ptr<DecodedEntry> entry; };
        auto pin = std::make_shared<ParserPin>(ParserPin{_state, entry});
        return {pin, &entry->lines[std::size_t(row)]};
    }
    if (mode == HistoryReadMode::Sequential) return {entry, &entry->lines[std::size_t(row)]};
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        for (auto it = r->lru.begin(); it != r->lru.end(); ++it) {
            if ((*it)->block.lock() == entry->block.lock() && (*it)->store == entry->store) {
                auto existing = *it; r->lru.splice(r->lru.end(), r->lru, it);
                return {existing, &existing->lines[std::size_t(row)]};
            }
        }
        r->lru.push_back(entry);
    }
    return {entry, &entry->lines[std::size_t(row)]};
}
PinnedHistoryLine HistoryReadContext::copy(const LogicalLine& line) {
    auto entry = std::make_shared<DecodedEntry>(); const auto r = _state->resources;
    {
        std::lock_guard<std::mutex> lock(r->mutex);
        const auto charge = std::size_t(line.byteSize()) + sizeof(DecodedEntry) + 128;
        if (!r->acquire(_state->decoded, charge)) return PinnedHistoryLine(HistoryReadStatus::Busy);
        entry->process = r->decoded; entry->store = _state->decoded; entry->bytes = charge; entry->released = r->released;
    }
    entry->lines.push_back(line); return {entry, &entry->lines.front()};
}
void HistoryReadContext::cancelCold() {
    std::lock_guard<std::mutex> lock(_state->callbacks); _state->cold.clear();
}
void HistoryReadContext::completeCold(const std::shared_ptr<const EncodedHistoryBlock>& block, const PinnedHistoryLine& result) {
    std::lock_guard<std::mutex> lock(_state->callbacks);
    auto found = std::find_if(_state->cold.begin(), _state->cold.end(), [&](const auto& item) { return item.block.lock() == block; });
    if (found == _state->cold.end()) return;
    if (result) _state->cold.erase(found);
    else { found->status = result.status(); found->error = result.error(); found->released = _state->resources->released->load(); }
}
void HistoryReadContext::setColdRequest(std::function<bool(std::shared_ptr<const EncodedHistoryBlock>)> request) {
    std::lock_guard<std::mutex> lock(_state->callbacks); _state->coldRequest = std::move(request);
}
void HistoryReadContext::setWake(std::function<void()> wake) { std::lock_guard<std::mutex> lock(_state->callbacks); _state->wake = std::move(wake); }
void HistoryReadContext::revokeWake() { std::lock_guard<std::mutex> lock(_state->callbacks); _state->wake = {}; _state->coldRequest = {}; }
void HistoryReadContext::notify() { std::lock_guard<std::mutex> lock(_state->callbacks); if (_state->wake) _state->wake(); }
} // namespace NovaTerm
