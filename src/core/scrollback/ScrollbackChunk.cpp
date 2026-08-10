/**
 * @file   ScrollbackChunk.cpp
 * @brief  滚动历史分块工具函数实现。
 */
#include "ScrollbackChunk.h"

namespace NovaTerm {

qsizetype estimateChunkBytes(const ScrollbackChunk& chunk)
{
    // 保守的分配器/控制块开销估算，作为上限估计而非 RSS 实测值。
    qsizetype bytes = sizeof(ScrollbackChunk) + 64;
    for (const LogicalLine& line : chunk.lines)
        bytes += line.byteSize();
    return bytes;
}

ScrollbackChunkPtr sealChunk(std::shared_ptr<ScrollbackChunk> chunk)
{
    if (!chunk)
        return {};
    chunk->byteSize = estimateChunkBytes(*chunk);
    chunk->sealed = true;
    // 转为 const 共享指针，使后续持有者无法修改分块内容，
    // 多个 ScrollbackSnapshot 可安全共享同一分块。
    return std::const_pointer_cast<const ScrollbackChunk>(std::move(chunk));
}

} // namespace NovaTerm
