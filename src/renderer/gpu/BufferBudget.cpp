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
    if (requiredBytes > _budget) {
        ++_statistics.budgetRejections;
        return std::nullopt;
    }
    quint64 next = std::max(_minimum, _capacity);
    while (next < requiredBytes) {
        const quint64 growth = std::max<quint64>(next / 2, 4096);
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
