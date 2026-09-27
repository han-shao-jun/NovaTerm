/**
 * @file   ScrollbackChunk.cpp
 * @brief  滚动历史分块工具函数实现。
 */
#include "ScrollbackChunk.h"

namespace NovaTerm {

isize estimateChunkBytes(const ScrollbackChunk& chunk)
{
    // 保守的分配器/控制块开销估算，作为上限估计而非 RSS 实测值。
    // lines 容器本身的堆分配（capacity × sizeof(LogicalLine)）也必须计入：
    // ensureActive 的 reserve(_chunkLines) 预留约 40 KB/块，不计数则
    // maxBytes 限额管不到它——回看期间每帧封存的几行碎块可累积
    // 数百 MB 未入账的预留。
    isize bytes = sizeof(ScrollbackChunk) + 64
        + isize(chunk.lines.capacity()) * isize(sizeof(LogicalLine));
    for (const LogicalLine& line : chunk.lines)
        bytes += line.byteSize();
    return bytes;
}

ScrollbackChunkPtr sealChunk(std::shared_ptr<ScrollbackChunk> chunk)
{
    if (!chunk)
        return {};
    // 封存块此后不可变：归还 lines 的多余预留。守卫保证正常路径
    // （写满 1024 行才封存，capacity == size）零拷贝零分配；只有回看期
    // sealActive 封存的几行碎块才真正收缩，每块收回约 40 KB 预留。
    if (chunk->lines.size() < chunk->lines.capacity())
        chunk->lines.shrink_to_fit();
    chunk->byteSize = estimateChunkBytes(*chunk);
    chunk->sealed = true;
    // 转为 const 共享指针，使后续持有者无法修改分块内容，
    // 多个 ScrollbackSnapshot 可安全共享同一分块。
    return std::const_pointer_cast<const ScrollbackChunk>(std::move(chunk));
}

} // namespace NovaTerm
