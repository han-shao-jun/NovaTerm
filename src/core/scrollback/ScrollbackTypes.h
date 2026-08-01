#pragma once

#include "core/terminal/TerminalTypes.h"

#include <QMetaType>
#include <QVector>

#include <cstdint>
#include <memory>

namespace NovaTerm {

using LineId = quint64;
using ChunkId = quint64;

// A logical line is terminated by a hard break. Lines with hardBreak=false
// may be joined to the following line by importers that can distinguish a
// terminal soft wrap. Cell::chars stores the base code point followed by its
// combining sequence; WideCharContinuation is never an independent glyph.
struct LogicalLine
{
    QVector<Cell> cells;
    bool hardBreak{true};
    LineId id{0};

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

struct ScrollbackChunk
{
    ChunkId id{0};
    QVector<LogicalLine> lines;
    qsizetype byteSize{0};
    bool sealed{false};
};

using ScrollbackChunkPtr = std::shared_ptr<const ScrollbackChunk>;

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
