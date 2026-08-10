/**
 * @file   BufferBudget.h
 * @brief  GPU 顶点/索引缓冲容量预算管理。
 *
 * 终端渲染器需要为每行的渲染命令分配 GPU 缓冲。BufferBudget 在固定
 * 内存预算内按需增长容量（capacity），避免每帧重新分配。当请求超过
 * 预算时返回 std::nullopt，调用方需退化为分批上传或丢弃。
 */
#pragma once

#include <QtGlobal>

#include <optional>

namespace NovaTerm {

/**
 * @brief 计算某行实际需要上传的顶点数。
 * @param activeVertexCount 该行当前活跃顶点数。
 * @param retainedStride    保留区步长（已分配但可能复用的顶点槽位数）。
 * @param fullRowUpload     是否整行重传。
 * @return fullRowUpload=true 时返回 retainedStride（重传整个保留区），
 *         否则返回 activeVertexCount（仅传活跃部分）。
 */
int rowUploadVertexCount(int activeVertexCount, int retainedStride,
                         bool fullRowUpload);

// 缓冲预算统计信息。
struct BufferBudgetStatistics
{
    quint64 currentBytes{0};       // 当前已分配容量
    quint64 peakBytes{0};           // 峰值容量
    quint64 reallocations{0};      // 扩容次数
    quint64 budgetRejections{0};   // 因超预算被拒的请求次数
};

// GPU 缓冲容量预算管理器。单线程使用。
class BufferBudget
{
public:
    explicit BufferBudget(quint64 budgetBytes = 64ull * 1024ull * 1024ull,
                          quint64 minimumBytes = 256ull * 1024ull);

    /**
     * @brief 请求至少 requiredBytes 容量。若当前容量足够直接返回；
     *        否则按 +50% 增长策略扩容直至满足或达到预算上限。
     * @param requiredBytes 需要的容量字节数。
     * @return 实际可用容量；超过预算上限返回 std::nullopt。
     */
    std::optional<quint64> capacityFor(quint64 requiredBytes);

    /**
     * @brief 释放已分配容量（置 0），但保留预算上限配置。
     *        用于 GPU 资源丢失后强制重建。
     */
    void release();
    quint64 capacity() const { return _capacity; }
    quint64 budget() const { return _budget; }
    const BufferBudgetStatistics& statistics() const { return _statistics; }

private:
    quint64 _budget;     // 容量上限
    quint64 _minimum;    // 初始/最小容量
    quint64 _capacity{0};  // 当前已分配容量
    BufferBudgetStatistics _statistics;
};

} // namespace NovaTerm
