/**
 * @file   ScrollbackSnapshot.cpp
 * @brief  滚动历史只读快照实现。
 *
 * 详见 ScrollbackSnapshot.h 的接口说明。本文件实现按文档行号或
 * 全局 LineId 的二分查找，所有查找都基于快照内已按 ID 升序排列的
 * 分块列表（ChunkedScrollback 在构建快照时保证）。
 */
#include "ScrollbackSnapshot.h"

#include <algorithm>

namespace NovaTerm {

namespace {

// 在已按 firstLineId 升序排列的分块列表中二分查找包含指定 LineId 的分块。
// 各分块内部的行 ID 也保证单调递增。返回分块索引，未命中返回 -1。
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

    // 按 documentStart 二分查找命中的分块。documentStart 在分块列表中
    // 单调递增，因此可视为标准的 lower_bound 查找。
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
    // 分块内部的行 ID 单调递增，用 lower_bound 在分块内二分定位。
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
