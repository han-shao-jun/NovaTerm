#pragma once

#include "ScrollbackTypes.h"

namespace NovaTerm {

qsizetype estimateChunkBytes(const ScrollbackChunk& chunk);
ScrollbackChunkPtr sealChunk(std::shared_ptr<ScrollbackChunk> chunk);

} // namespace NovaTerm
