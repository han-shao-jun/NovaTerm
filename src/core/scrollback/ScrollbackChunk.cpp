#include "ScrollbackChunk.h"

namespace NovaTerm {

qsizetype estimateChunkBytes(const ScrollbackChunk& chunk)
{
    // Conservative allocator/control-block charge. This is an accounted
    // upper-bound estimate rather than an RSS measurement.
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
    return std::const_pointer_cast<const ScrollbackChunk>(std::move(chunk));
}

} // namespace NovaTerm
