/** @file HistoryCompressionService.h
 * @brief 进程共享的有界历史服务；存储只在自身单写线程应用完成结果。
 */
#pragma once
#include "HistoryReadContext.h"
#include <functional>
namespace NovaTerm {
struct HistoryCompressionPolicy {
    bool enabled{false};
    std::size_t targetBlockBytes{512 * 1024};
    std::size_t maximumBlockBytes{1024 * 1024};
    std::size_t hotBytes{4 * 1024 * 1024};
    std::size_t minimumBlockBytes{32 * 1024};
};
struct HistoryServiceStatistics {
    std::size_t pendingTasks{0}, outstandingTasks{0}, queuedSourceBytes{0}, queuedResultBytes{0}, queueHighWater{0};
    u64 compressedBlocks{0}, skippedBlocks{0}, failures{0}, rejectedTasks{0};
    std::size_t liveHistoryBytes{0}, retainedHistoryBytes{0}, residentHistoryCharge{0};
    HistoryResourceStatistics resources;
};
class HistoryRegistration;
struct HistoryCompletion {
    ScrollbackChunkPtr source;
    std::shared_ptr<const EncodedHistoryBlock> encoded;
    u64 epoch{0};
    std::string error;
};
class HistoryCompressionService {
public:
    explicit HistoryCompressionService(HistoryResourceLimits limits = {});
    ~HistoryCompressionService();
    HistoryCompressionService(const HistoryCompressionService&) = delete;
    HistoryCompressionService& operator=(const HistoryCompressionService&) = delete;
    /** @brief 预留 Parser 槽失败则返回空；唤醒函数必须不阻塞、不抛出。 */
    std::shared_ptr<HistoryRegistration> registerStore(std::function<void()> wake);
    /** @brief 诊断租约不预留 Parser 槽，关闭压缩的终端也纳入进程总量。 */
    std::shared_ptr<HistoryAllocationCharge> trackPayload(std::size_t bytes);
    u64 registerDiagnostic();
    void updateDiagnostic(u64 id, std::size_t live, std::size_t retained);
    void removeDiagnostic(u64 id);
    /** @brief 无等待提交；队列满返回 false，调用者保留原始历史。 */
    bool submit(const std::shared_ptr<HistoryRegistration>& registration, ScrollbackChunkPtr source);
    bool take(const std::shared_ptr<HistoryRegistration>& registration, HistoryCompletion& completion);
    /** @brief clear/策略代次变更：丢弃等待任务和结果，迟到结果不会复活历史。 */
    void invalidate(const std::shared_ptr<HistoryRegistration>& registration);
    /** @brief 撤销通知，返回后不会调用原 wake；不等待正在编码的块。 */
    void unregisterStore(const std::shared_ptr<HistoryRegistration>& registration);
    /** @brief 停止接收并丢弃等待任务，仅 join 当前单块操作；可重复调用。 */
    void stop();
    [[nodiscard]] HistoryServiceStatistics statistics() const;
private:
    struct Impl;
    std::shared_ptr<Impl> _impl;
};
class HistoryRegistration {
public:
    [[nodiscard]] std::shared_ptr<HistoryReadContext> reader() const;
    [[nodiscard]] u64 epoch() const;
private:
    struct State;
    std::shared_ptr<State> _state;
    friend class HistoryCompressionService;
};
} // namespace NovaTerm
