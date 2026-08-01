#pragma once

#include "ScrollbackSnapshot.h"

#include <deque>
#include <memory>
#include <vector>

namespace NovaTerm {

class ChunkedScrollback
{
public:
    static constexpr qsizetype DefaultChunkLines = 1024;
    static constexpr qsizetype DefaultMaxLines = 100'000;
    static constexpr qsizetype MaximumMaxLines = 1'000'000;
    static constexpr qsizetype DefaultMaxBytes = 256 * 1024 * 1024;

    explicit ChunkedScrollback(qsizetype maxLines = DefaultMaxLines,
                               qsizetype maxBytes = DefaultMaxBytes,
                               qsizetype chunkLines = DefaultChunkLines);

    LineId append(LogicalLine line);
    LineId appendContinuation(LogicalLine fragment);
    LineId append(const Cell* cells, qsizetype columns,
                  bool hardBreak = true);
    bool popOldest(LogicalLine& line);
    void publish();
    void clear();

    // Snapshot is a publication boundary: the active tail is sealed and then
    // shared. No historical Cell storage is copied.
    ScrollbackSnapshot snapshot();
    const LogicalLine* lineAt(qsizetype index) const;
    qsizetype lineCount() const { return _lineCount; }
    qsizetype maxLines() const { return _maxLines; }
    qsizetype maxBytes() const { return _maxBytes; }
    qsizetype chunkLines() const { return _chunkLines; }
    quint64 version() const { return _version; }

    void setLimits(qsizetype maxLines, qsizetype maxBytes);
    ScrollbackStatistics statistics() const;

private:
    struct StoredChunk
    {
        ScrollbackChunkPtr chunk;
        qsizetype firstLine{0};
        qsizetype effectiveBytes{0};
    };
    struct RetiredChunk
    {
        std::weak_ptr<const ScrollbackChunk> chunk;
        qsizetype bytes{0};
    };

    void ensureActive();
    void sealActive();
    void enforceLimits();
    void evictOldest();
    static qsizetype lineBytes(const LogicalLine& line);
    void collectRetired() const;

    std::deque<StoredChunk> _chunks;
    std::shared_ptr<ScrollbackChunk> _active;
    qsizetype _activeFirstLine{0};
    qsizetype _activeBytes{0};
    qsizetype _lineCount{0};
    qsizetype _cellCount{0};
    qsizetype _effectiveBytes{0};
    qsizetype _maxLines{DefaultMaxLines};
    qsizetype _maxBytes{DefaultMaxBytes};
    qsizetype _chunkLines{DefaultChunkLines};
    LineId _nextLineId{1};
    ChunkId _nextChunkId{1};
    quint64 _version{0};
    quint64 _evictedLines{0};
    quint64 _evictedChunks{0};
    mutable std::vector<RetiredChunk> _retired;
};

} // namespace NovaTerm
