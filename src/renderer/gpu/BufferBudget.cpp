/**
 * @file   BufferBudget.cpp
 * @brief  GPU 缓冲容量预算管理实现。
 *
 * 详见 BufferBudget.h。capacityFor() 实现按 +50% 增长（最少 4096 字节）
 * 的扩容策略，并在溢出 uint64 时安全拒绝。
 */
#include "BufferBudget.h"

#include <algorithm>
#include <limits>

namespace NovaTerm {

int rowUploadVertexCount(int activeVertexCount, int retainedStride,
                         bool fullRowUpload)
{
    activeVertexCount = std::max(0, activeVertexCount);
    retainedStride = std::max(activeVertexCount, retainedStride);
    return fullRowUpload ? retainedStride : activeVertexCount;
}

BufferBudget::BufferBudget(quint64 budgetBytes, quint64 minimumBytes)
    : _budget(std::max<quint64>(1, budgetBytes))
    , _minimum(std::min(std::max<quint64>(1, minimumBytes), _budget))
{
}

std::optional<quint64> BufferBudget::capacityFor(quint64 requiredBytes)
{
    if (requiredBytes <= _capacity)
        return _capacity;
    // 超过预算上限：无法满足，调用方需退化为分批或丢弃。
    if (requiredBytes > _budget) {
        ++_statistics.budgetRejections;
        return std::nullopt;
    }
    // 从 minimum 或当前 capacity 起按 +50% 增长，最少 4096 字节。
    quint64 next = std::max(_minimum, _capacity);
    while (next < requiredBytes) {
        const quint64 growth = std::max<quint64>(next / 2, 4096);
        // 溢出保护：uint64 加法溢出时直接拒绝，避免回绕到极小值。
        if (next > std::numeric_limits<quint64>::max() - growth) {
            ++_statistics.budgetRejections;
            return std::nullopt;
        }
        next += growth;
    }
    next = std::min(next, _budget);
    if (next < requiredBytes) {
        ++_statistics.budgetRejections;
        return std::nullopt;
    }
    _capacity = next;
    _statistics.currentBytes = next;
    _statistics.peakBytes = std::max(_statistics.peakBytes, next);
    ++_statistics.reallocations;
    return next;
}

void BufferBudget::release()
{
    _capacity = 0;
    _statistics.currentBytes = 0;
}

} // namespace NovaTerm
