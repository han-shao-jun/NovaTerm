/** @file HistoryReadContext.h
 * @brief 显式共享预算、缓存与每存储 Parser 槽；不依赖应用或 Qt。
 */
#pragma once
#include "ScrollbackBlockCodec.h"
#include "core/scrollback/PinnedHistoryLine.h"
#include <functional>
#include <atomic>
#include <memory>
namespace NovaTerm {
enum class HistoryReadMode { Synchronous, CacheOnly, Parser, Sequential };
struct HistoryResourceLimits {
    std::size_t processDecodedBytes{32 * 1024 * 1024};
    std::size_t storeDecodedBytes{8 * 1024 * 1024};
    std::size_t temporaryBytes{16 * 1024 * 1024};
    std::size_t parserSlotBytes{3 * 1024 * 1024};
    std::size_t workerSlotBytes{4 * 1024 * 1024};
};
struct HistoryResourceStatistics {
    std::size_t decodedBytes{0};
    std::size_t payloadResidentBytes{0};
    std::size_t pinnedDecodedBytes{0};
    std::size_t reservedTemporaryBytes{0};
    u64 cacheHits{0}, cacheMisses{0}, busyReads{0}, decodeErrors{0};
};
/** @brief 分配生命周期 charge；快照/任务共享同一 token，不重复计数。 */
class HistoryAllocationCharge {
public:
    explicit HistoryAllocationCharge(std::shared_ptr<std::atomic<std::size_t>> total) : _total(std::move(total)) {}
    ~HistoryAllocationCharge() { _total->fetch_sub(_bytes); }
    void set(std::size_t bytes) {
        if (bytes >= _bytes) _total->fetch_add(bytes - _bytes); else _total->fetch_sub(_bytes - bytes);
        _bytes = bytes;
    }
private:
    std::shared_ptr<std::atomic<std::size_t>> _total;
    std::size_t _bytes{0};
};
class HistoryReadContext;
class HistoryResources {
public:
    explicit HistoryResources(HistoryResourceLimits limits = {});
    ~HistoryResources();
    /** @brief 槽不足时返回空，调用者保持 Raw；没有隐式全局实例。 */
    std::shared_ptr<HistoryReadContext> createContext();
    [[nodiscard]] HistoryResourceStatistics statistics() const;
    /** @brief 只释放未 pin 的缓存；已 pin 的分配继续计入限额。 */
    void trim();
    std::shared_ptr<HistoryAllocationCharge> trackPayload(std::size_t bytes);
private:
    struct State;
    std::shared_ptr<State> _state;
    friend class HistoryReadContext;
};
class HistoryReadContext : public std::enable_shared_from_this<HistoryReadContext> {
public:
    ~HistoryReadContext();
    /** @brief 同步只解码一个普通块；CacheOnly 未命中返回 Pending/Busy。 */
    PinnedHistoryLine read(std::shared_ptr<const EncodedHistoryBlock> block,
                          isize row, HistoryReadMode mode);
    /** @brief 复制活动行也申请相同的进程/存储解码额度。 */
    PinnedHistoryLine copy(const LogicalLine& line);
    void cancelCold();
    void completeCold(const std::shared_ptr<const EncodedHistoryBlock>& block, const PinnedHistoryLine& result);
    void setColdRequest(std::function<bool(std::shared_ptr<const EncodedHistoryBlock>)> request);
    void setWake(std::function<void()> wake);
    /** @brief 线性撤销通知；返回后没有在途 wake。回调必须不阻塞。 */
    void revokeWake();
    void notify();
    [[nodiscard]] HistoryResourceStatistics statistics() const;
private:
    struct State;
    explicit HistoryReadContext(std::shared_ptr<HistoryResources::State> resources);
    std::shared_ptr<State> _state;
    friend class HistoryResources;
};
} // namespace NovaTerm
