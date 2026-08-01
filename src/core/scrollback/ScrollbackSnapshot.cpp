#include "ScrollbackSnapshot.h"

#include <algorithm>

namespace NovaTerm {

namespace {

qsizetype chunkIndexForId(const QVector<ScrollbackSnapshot::ChunkView>& chunks,
                          LineId id)
{
    qsizetype low = 0;
    qsizetype high = chunks.size();
    while (low < high) {
        const qsizetype middle = low + (high - low) / 2;
        const auto& view = chunks[middle];
        const auto begin = view.chunk->lines.cbegin() + view.firstLine;
        const LineId first = begin->id;
        const LineId last = (begin + view.lineCount - 1)->id;
        if (id < first)
            high = middle;
        else if (id > last)
            low = middle + 1;
        else
            return middle;
    }
    return -1;
}

} // namespace

const LogicalLine* ScrollbackSnapshot::lineAt(qsizetype documentRow) const
{
    if (documentRow < 0 || documentRow >= _lineCount)
        return nullptr;

    qsizetype low = 0;
    qsizetype high = _chunks.size();
    while (low < high) {
        const qsizetype middle = low + (high - low) / 2;
        const ChunkView& view = _chunks[middle];
        if (documentRow < view.documentStart) {
            high = middle;
        } else if (documentRow >= view.documentStart + view.lineCount) {
            low = middle + 1;
        } else {
            return &view.chunk->lines[view.firstLine
                                      + documentRow - view.documentStart];
        }
    }
    return nullptr;
}

const LogicalLine* ScrollbackSnapshot::lineById(LineId id) const
{
    if (_lineCount == 0 || id < _firstLineId || id > _lastLineId)
        return nullptr;
    const qsizetype index = chunkIndexForId(_chunks, id);
    if (index < 0)
        return nullptr;
    const ChunkView& view = _chunks[index];
    const auto begin = view.chunk->lines.cbegin() + view.firstLine;
    const auto end = begin + view.lineCount;
    const auto found = std::lower_bound(
        begin, end, id,
        [](const LogicalLine& line, LineId value) { return line.id < value; });
    return found != end && found->id == id ? &*found : nullptr;
}

qsizetype ScrollbackSnapshot::rowForLineId(LineId id) const
{
    if (_lineCount == 0 || id < _firstLineId || id > _lastLineId)
        return -1;
    const qsizetype index = chunkIndexForId(_chunks, id);
    if (index < 0)
        return -1;
    const ChunkView& view = _chunks[index];
    const auto begin = view.chunk->lines.cbegin() + view.firstLine;
    const auto end = begin + view.lineCount;
    const auto found = std::lower_bound(
        begin, end, id,
        [](const LogicalLine& line, LineId value) { return line.id < value; });
    if (found != end && found->id == id)
        return view.documentStart + (found - begin);
    return -1;
}

} // namespace NovaTerm
