/**
 * @file   ScrollbackChunk.h
 * @brief  滚动历史分块的工具函数。
 */
#pragma once

#include "ScrollbackTypes.h"

namespace NovaTerm {

/**
 * @brief 估算分块占用字节数（含分配器开销）。
 * @param chunk 待估算的分块。
 * @return 字节估算值，作为容量上限而非 RSS 实测值。
 */
qsizetype estimateChunkBytes(const ScrollbackChunk& chunk);

/**
 * @brief 封存分块：计算字节数并标记为不可变，转为 const 共享指针。
 * @param chunk 待封存的分块，可为空。
 * @return 封存后的 const 共享指针；输入为空时返回空指针。
 */
ScrollbackChunkPtr sealChunk(std::shared_ptr<ScrollbackChunk> chunk);

} // namespace NovaTerm
