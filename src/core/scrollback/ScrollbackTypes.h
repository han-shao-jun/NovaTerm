/**
 * @file   ScrollbackTypes.h
 * @brief  滚动历史共享类型定义。
 *
 * 定义逻辑行（LogicalLine）、分块（ScrollbackChunk）与统计信息
 * （ScrollbackStatistics）等数据结构，被 ChunkedScrollback 与
 * ScrollbackSnapshot 共享使用。
 */
#pragma once

#include "core/terminal/TerminalTypes.h"

#include <QMetaType>
#include <QVector>

#include <cstdint>
#include <memory>

namespace NovaTerm {

// 逻辑行全局唯一 ID，单调递增；0 表示无效。
using LineId = quint64;
// 分块全局唯一 ID，单调递增；0 表示无效。
using ChunkId = quint64;

// 逻辑行：终端输出的一个语义行，可能由多个软换行片段拼接而成。
// hardBreak=true 表示该行以硬换行（\n）结尾；false 表示它是软换行
// （终端宽度截断），后续行可与之拼接为同一逻辑行。
// Cell::chars 存储基础码点及其后的组合序列；WideCharContinuation
// 不会作为独立字形出现，仅占用下一格。
struct LogicalLine
{
    QVector<Cell> cells;
    bool hardBreak{true};
    LineId id{0};

    // 估算该行在内存中占用字节数。基于 cells.capacity() 而非 size()，
    // 以反映 QVector 已分配但未使用的尾部容量；计入分配器开销与
    // 16 字节对齐填充，作为容量估算而非 RSS 实测值。
    qsizetype byteSize() const
    {
        constexpr qsizetype AllocationOverhead = 64;
        constexpr qsizetype Alignment = 16;
        const qsizetype payload =
            cells.capacity() * qsizetype(sizeof(Cell));
        const qsizetype allocated = payload > 0
            ? ((payload + Alignment - 1) / Alignment) * Alignment
                + AllocationOverhead
            : 0;
        return qsizetype(sizeof(LogicalLine))
            + allocated;
    }
};

// 滚动历史分块：多个 LogicalLine 打包为一个分块，便于批量共享与回收。
// sealed=true 表示该分块已封存不可变，可被多个 ScrollbackSnapshot
// 以 shared_ptr 形式共享持有。
struct ScrollbackChunk
{
    ChunkId id{0};
    QVector<LogicalLine> lines;
    qsizetype byteSize{0};
    bool sealed{false};
};

using ScrollbackChunkPtr = std::shared_ptr<const ScrollbackChunk>;

// 滚动历史统计：供 UI 显示当前缓冲规模与回收情况。
struct ScrollbackStatistics
{
    quint64 version{0};
    qsizetype logicalLines{0};
    qsizetype logicalCells{0};
    qsizetype effectiveBytes{0};
    qsizetype retainedBySnapshots{0};
    qsizetype sealedChunks{0};
    qsizetype activeLines{0};
    quint64 evictedLines{0};
    quint64 evictedChunks{0};
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::LineId)
Q_DECLARE_METATYPE(NovaTerm::ScrollbackStatistics)
