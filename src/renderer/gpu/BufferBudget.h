#pragma once

#include <QtGlobal>

#include <optional>

namespace NovaTerm {

int rowUploadVertexCount(int activeVertexCount, int retainedStride,
                         bool fullRowUpload);

struct BufferBudgetStatistics
{
    quint64 currentBytes{0};
    quint64 peakBytes{0};
    quint64 reallocations{0};
    quint64 budgetRejections{0};
};

class BufferBudget
{
public:
    explicit BufferBudget(quint64 budgetBytes = 64ull * 1024ull * 1024ull,
                          quint64 minimumBytes = 256ull * 1024ull);
    std::optional<quint64> capacityFor(quint64 requiredBytes);
    void release();
    quint64 capacity() const { return _capacity; }
    quint64 budget() const { return _budget; }
    const BufferBudgetStatistics& statistics() const { return _statistics; }

private:
    quint64 _budget;
    quint64 _minimum;
    quint64 _capacity{0};
    BufferBudgetStatistics _statistics;
};

} // namespace NovaTerm
