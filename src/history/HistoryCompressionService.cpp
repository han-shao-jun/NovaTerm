/** @file HistoryCompressionService.cpp
 * @brief 单 worker、公平轮转、有界任务与完成 mailbox；不访问宿主对象。
 */
#include "HistoryCompressionService.h"
#include "core/scrollback/ScrollbackChunk.h"
#include "core/ThreadNaming.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <algorithm>
#include <unordered_map>
namespace NovaTerm {
struct HistoryRegistration::State {
    std::shared_ptr<HistoryReadContext> reader;
    std::atomic<u64> epoch{1};
    bool active{true};
    std::size_t outstanding{0};
    std::deque<HistoryCompletion> results;
};
struct HistoryCompressionService::Impl : std::enable_shared_from_this<Impl> {
    explicit Impl(HistoryResourceLimits limits) : resources(limits) {}
    struct Task {
        std::shared_ptr<HistoryRegistration::State> registration;
        ScrollbackChunkPtr source;
        std::shared_ptr<const EncodedHistoryBlock> decode;
        u64 epoch{0};
        std::size_t charge{0};
    };
    HistoryResources resources;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> pending;
    std::vector<std::weak_ptr<HistoryRegistration::State>> registrations;
    HistoryServiceStatistics stats;
    u64 nextDiagnostic{1};
    std::unordered_map<u64, std::pair<std::size_t, std::size_t>> diagnostics;
    HistoryRegistration::State* previous{nullptr};
    bool stopping{false};
    std::thread worker;
    void release(const Task& task) {
        --stats.outstandingTasks; --task.registration->outstanding; stats.queuedSourceBytes -= task.charge;
    }
    bool enqueue(Task task) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || !task.registration->active || stats.outstandingTasks >= 32
            || task.registration->outstanding >= (task.decode ? 3u : 2u) || task.charge > 16 * 1024 * 1024 - stats.queuedSourceBytes) {
            ++stats.rejectedTasks; return false;
        }
        if (task.decode) {
            for (const auto& item : pending) if (item.registration == task.registration && item.decode == task.decode) return true;
        }
        task.epoch = task.registration->epoch.load();
        ++stats.outstandingTasks; ++task.registration->outstanding; stats.queuedSourceBytes += task.charge;
        pending.push_back(std::move(task)); stats.queueHighWater = std::max(stats.queueHighWater, pending.size());
        changed.notify_one(); return true;
    }
    void run() {
        setCurrentThreadName("nvterm-history");
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || !pending.empty(); });
                if (stopping) return;
                // 可见视口的冷读优先；同优先级按存储轮转，压缩不占满冷读槽。
                auto it = std::find_if(pending.begin(), pending.end(), [this](const Task& item) { return item.decode && item.registration.get() != previous; });
                if (it == pending.end()) it = std::find_if(pending.begin(), pending.end(), [](const Task& item) { return bool(item.decode); });
                if (it == pending.end()) it = std::find_if(pending.begin(), pending.end(), [this](const Task& item) { return item.registration.get() != previous; });
                if (it == pending.end()) it = pending.begin();
                task = std::move(*it); pending.erase(it); previous = task.registration.get();
            }
            HistoryCompletion result; result.source = task.source; result.epoch = task.epoch;
            try {
                if (task.epoch == task.registration->epoch.load()) {
                    if (task.decode) {
                        const auto loaded = task.registration->reader->read(task.decode, 0, HistoryReadMode::Synchronous);
                        task.registration->reader->completeCold(task.decode, loaded);
                        if (loaded.status() == HistoryReadStatus::DecodeError) result.error = loaded.error();
                    } else {
                        auto encoded = ScrollbackBlockCodec::encode(task.source->lines);
                        if (!encoded) result.error = encoded.error;
                        else {
                            const auto cost = encoded.block.byteSize() + sizeof(ScrollbackChunk) + 64;
                            const auto raw = std::size_t(task.source->byteSize);
                            if (cost + 4096 <= raw && cost <= raw - raw / 8)
                                { encoded.block.originalCharge = raw; result.encoded = std::make_shared<const EncodedHistoryBlock>(std::move(encoded.block)); }
                        }
                    }
                }
            } catch (const std::exception& e) { result.error = e.what(); }
              catch (...) { result.error = "unknown history worker failure"; }
            bool notify = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!result.error.empty()) ++stats.failures;
                else if (result.encoded) ++stats.compressedBlocks;
                else if (!task.decode) ++stats.skippedBlocks;
                if (!stopping && task.registration->active && task.epoch == task.registration->epoch.load()) {
                    if (!task.decode) {
                        // 完成 payload 共用 4 MiB worker 预留中的 1 MiB；其余
                        // 空间留给当前单块编码峰值。满即保留原块，不额外分配。
                        if (result.encoded && result.encoded->byteSize() > 1024 * 1024 - stats.queuedResultBytes) {
                            result.encoded.reset(); ++stats.skippedBlocks;
                        }
                        if (result.encoded) stats.queuedResultBytes += result.encoded->byteSize();
                        task.registration->results.push_back(std::move(result));
                    }
                    else release(task);
                    notify = true;
                } else release(task);
            }
            if (notify) task.registration->reader->notify();
        }
    }
};
HistoryCompressionService::HistoryCompressionService(HistoryResourceLimits limits) : _impl(std::make_shared<Impl>(limits)) {
    _impl->worker = std::thread([impl = _impl] { impl->run(); });
}
HistoryCompressionService::~HistoryCompressionService() { stop(); }
std::shared_ptr<HistoryRegistration> HistoryCompressionService::registerStore(std::function<void()> wake) {
    auto context = _impl->resources.createContext(); if (!context) return {};
    auto r = std::make_shared<HistoryRegistration>(); r->_state = std::make_shared<HistoryRegistration::State>(); r->_state->reader = context;
    context->setWake(std::move(wake));
    std::weak_ptr<Impl> weak = _impl; std::weak_ptr<HistoryRegistration::State> registration = r->_state;
    context->setColdRequest([weak, registration](std::shared_ptr<const EncodedHistoryBlock> block) {
        const auto impl = weak.lock(); const auto state = registration.lock();
        return impl && state && impl->enqueue({state, {}, std::move(block), 0, 0});
    });
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        if (_impl->stopping) { context->revokeWake(); return {}; }
        _impl->registrations.erase(std::remove_if(_impl->registrations.begin(), _impl->registrations.end(), [](const auto& item) { return item.expired(); }), _impl->registrations.end());
        _impl->registrations.push_back(r->_state);
    }
    return r;
}
std::shared_ptr<HistoryAllocationCharge> HistoryCompressionService::trackPayload(std::size_t bytes) {
    return _impl->resources.trackPayload(bytes);
}
u64 HistoryCompressionService::registerDiagnostic() {
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (_impl->stopping) return 0;
    const auto id = _impl->nextDiagnostic++; _impl->diagnostics.emplace(id, std::make_pair(0, 0)); return id;
}
void HistoryCompressionService::updateDiagnostic(u64 id, std::size_t live, std::size_t retained) {
    if (!id) return;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    auto it = _impl->diagnostics.find(id); if (it != _impl->diagnostics.end()) it->second = {live, retained};
}
void HistoryCompressionService::removeDiagnostic(u64 id) {
    std::lock_guard<std::mutex> lock(_impl->mutex); _impl->diagnostics.erase(id);
}
bool HistoryCompressionService::submit(const std::shared_ptr<HistoryRegistration>& r, ScrollbackChunkPtr source) {
    if (!r || !source || source->encoded || source->byteSize > 1024 * 1024 || source->lines.size() > 1024) return false;
    return _impl->enqueue({r->_state, source, {}, 0, std::size_t(source->byteSize)});
}
bool HistoryCompressionService::take(const std::shared_ptr<HistoryRegistration>& r, HistoryCompletion& result) {
    if (!r) return false;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (r->_state->results.empty()) return false;
    result = std::move(r->_state->results.front()); r->_state->results.pop_front();
    if (result.encoded) _impl->stats.queuedResultBytes -= result.encoded->byteSize();
    _impl->release({r->_state, result.source, {}, result.epoch, std::size_t(result.source->byteSize)});
    return true;
}
void HistoryCompressionService::invalidate(const std::shared_ptr<HistoryRegistration>& r) {
    if (!r) return;
    r->_state->reader->cancelCold();
    std::lock_guard<std::mutex> lock(_impl->mutex); ++r->_state->epoch;
    for (auto it = _impl->pending.begin(); it != _impl->pending.end();) {
        if (it->registration == r->_state) { _impl->release(*it); it = _impl->pending.erase(it); } else ++it;
    }
    for (const auto& result : r->_state->results) {
        if (result.encoded) _impl->stats.queuedResultBytes -= result.encoded->byteSize();
        _impl->release({r->_state, result.source, {}, result.epoch, std::size_t(result.source->byteSize)});
    }
    r->_state->results.clear();
}
void HistoryCompressionService::unregisterStore(const std::shared_ptr<HistoryRegistration>& r) {
    if (!r) return;
    // 先线性撤销 wake，再锁服务；服务线程从不在服务锁内调用 wake。
    r->_state->reader->revokeWake(); invalidate(r);
    std::lock_guard<std::mutex> lock(_impl->mutex); r->_state->active = false;
}
void HistoryCompressionService::stop() {
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        if (!_impl->stopping) {
            _impl->stopping = true;
            for (const auto& task : _impl->pending) _impl->release(task);
            _impl->pending.clear();
        }
    }
    _impl->changed.notify_all();
    if (_impl->worker.joinable()) _impl->worker.join();
    for (const auto& weak : _impl->registrations) if (const auto r = weak.lock()) {
        r->reader->revokeWake();
        std::lock_guard<std::mutex> lock(_impl->mutex);
        for (const auto& result : r->results) {
            if (result.encoded) _impl->stats.queuedResultBytes -= result.encoded->byteSize();
            _impl->release({r, result.source, {}, result.epoch, std::size_t(result.source->byteSize)});
        }
        r->results.clear(); r->active = false;
    }
    _impl->resources.trim();
}
HistoryServiceStatistics HistoryCompressionService::statistics() const {
    std::lock_guard<std::mutex> lock(_impl->mutex); auto result = _impl->stats;
    result.pendingTasks = _impl->pending.size(); result.resources = _impl->resources.statistics();
    for (const auto& owner : _impl->diagnostics) { result.liveHistoryBytes += owner.second.first; result.retainedHistoryBytes += owner.second.second; }
    result.residentHistoryCharge = result.resources.payloadResidentBytes
        + result.resources.decodedBytes + result.resources.reservedTemporaryBytes;
    return result;
}
std::shared_ptr<HistoryReadContext> HistoryRegistration::reader() const { return _state->reader; }
u64 HistoryRegistration::epoch() const { return _state->epoch.load(); }
} // namespace NovaTerm
