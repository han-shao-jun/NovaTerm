#pragma once

#include "ScrollbackTypes.h"

#include <QVector>

namespace NovaTerm {

class ChunkedScrollback;

class ScrollbackSnapshot
{
public:
    struct ChunkView
    {
        ScrollbackChunkPtr chunk;
        qsizetype firstLine{0};
        qsizetype lineCount{0};
        qsizetype documentStart{0};
    };

    quint64 version() const { return _version; }
    qsizetype lineCount() const { return _lineCount; }
    LineId firstLineId() const { return _firstLineId; }
    LineId lastLineId() const { return _lastLineId; }
    bool empty() const { return _lineCount == 0; }

    const LogicalLine* lineAt(qsizetype documentRow) const;
    const LogicalLine* lineById(LineId id) const;
    qsizetype rowForLineId(LineId id) const;
    bool contains(LineId id) const { return rowForLineId(id) >= 0; }
    const QVector<ChunkView>& chunks() const { return _chunks; }

private:
    friend class ChunkedScrollback;
    QVector<ChunkView> _chunks;
    quint64 _version{0};
    qsizetype _lineCount{0};
    LineId _firstLineId{0};
    LineId _lastLineId{0};
};

} // namespace NovaTerm
