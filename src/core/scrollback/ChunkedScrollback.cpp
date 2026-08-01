#include "ChunkedScrollback.h"

#include "ScrollbackChunk.h"

#include <algorithm>
#include <limits>

namespace NovaTerm {

namespace {
constexpr qsizetype ChunkAllocationOverhead = 64;
}

ChunkedScrollback::ChunkedScrollback(qsizetype maxLines, qsizetype maxBytes,
                                     qsizetype chunkLines)
    : _maxLines(std::clamp<qsizetype>(maxLines, 0, MaximumMaxLines))
    , _maxBytes(std::max<qsizetype>(0, maxBytes))
    , _chunkLines(std::max<qsizetype>(1, chunkLines))
{
}

qsizetype ChunkedScrollback::lineBytes(const LogicalLine& line)
{
    return line.byteSize();
}

void ChunkedScrollback::ensureActive()
{
    if (_active)
        return;
    _active = std::make_shared<ScrollbackChunk>();
    _active->id = _nextChunkId++;
    _active->lines.reserve(_chunkLines);
    _activeFirstLine = 0;
    _activeBytes = sizeof(ScrollbackChunk) + ChunkAllocationOverhead;
    _effectiveBytes += _activeBytes;
}

LineId ChunkedScrollback::append(LogicalLine line)
{
    // IDs belong to this document, not to callers. This preserves strict
    // monotonic ordering required by snapshot lookup across eviction.
    line.id = _nextLineId++;
    const LineId id = line.id;

    ensureActive();
    const qsizetype bytes = lineBytes(line);
    _cellCount += line.cells.size();
    _lineCount++;
    _activeBytes += bytes;
    _effectiveBytes += bytes;
    _active->lines.push_back(std::move(line));
    ++_version;

    if (_active->lines.size() >= _chunkLines)
        sealActive();
    enforceLimits();
    return id;
}

LineId ChunkedScrollback::appendContinuation(LogicalLine fragment)
{
    if (_lineCount == 0)
        return append(std::move(fragment));
    const qsizetype addedCells = fragment.cells.size();
    LineId id = 0;
    if (_active && _activeFirstLine < _active->lines.size()) {
        LogicalLine& line = _active->lines.last();
        const qsizetype before = lineBytes(line);
        line.cells += fragment.cells;
        line.hardBreak = fragment.hardBreak;
        const qsizetype delta = lineBytes(line) - before;
        _activeBytes += delta;
        _effectiveBytes += delta;
        id = line.id;
    } else if (!_chunks.empty()) {
        StoredChunk& stored = _chunks.back();
        const ScrollbackChunkPtr previous = stored.chunk;
        auto replacement = std::make_shared<ScrollbackChunk>(*previous);
        replacement->id = _nextChunkId++;
        replacement->sealed = false;
        LogicalLine& line = replacement->lines.last();
        line.cells += fragment.cells;
        line.hardBreak = fragment.hardBreak;
        id = line.id;
        const ScrollbackChunkPtr sealed = sealChunk(std::move(replacement));
        _effectiveBytes += sealed->byteSize - previous->byteSize;
        stored.chunk = sealed;
        stored.effectiveBytes += sealed->byteSize - previous->byteSize;
        _retired.push_back({previous, previous->byteSize});
    }
    _cellCount += addedCells;
    ++_version;
    enforceLimits();
    return id;
}

LineId ChunkedScrollback::append(const Cell* cells, qsizetype columns,
                                 bool hardBreak)
{
    LogicalLine line;
    line.hardBreak = hardBreak;
    if (cells && columns > 0)
        line.cells = QVector<Cell>(cells, cells + columns);
    return append(std::move(line));
}

void ChunkedScrollback::sealActive()
{
    if (!_active || _activeFirstLine >= _active->lines.size()) {
        _active.reset();
        _activeFirstLine = 0;
        _activeBytes = 0;
        return;
    }
    const qsizetype skippedBytes = [&]() {
        qsizetype value = 0;
        for (qsizetype i = 0; i < _activeFirstLine; ++i)
            value += lineBytes(_active->lines[i]);
        return value;
    }();
    ScrollbackChunkPtr sealed = sealChunk(std::move(_active));
    _chunks.push_back({sealed, _activeFirstLine,
                       qsizetype(sealed->byteSize
                                 - qsizetype(sizeof(ScrollbackChunk))
                                 - ChunkAllocationOverhead
                                 - skippedBytes)});
    _activeFirstLine = 0;
    _activeBytes = 0;
}

void ChunkedScrollback::publish()
{
    if (_active && _activeFirstLine < _active->lines.size())
        sealActive();
}

void ChunkedScrollback::evictOldest()
{
    if (_lineCount == 0)
        return;

    bool evicted = false;
    qsizetype oldestCellCount = 0;
    if (!_chunks.empty()) {
        StoredChunk& stored = _chunks.front();
        oldestCellCount = stored.chunk->lines[stored.firstLine].cells.size();
        evicted = true;
        ++stored.firstLine;
        if (stored.firstLine >= stored.chunk->lines.size()) {
            _effectiveBytes -= stored.chunk->byteSize;
            _retired.push_back({stored.chunk, stored.chunk->byteSize});
            _chunks.pop_front();
            ++_evictedChunks;
        }
    } else if (_active && _activeFirstLine < _active->lines.size()) {
        oldestCellCount = _active->lines[_activeFirstLine].cells.size();
        evicted = true;
        ++_activeFirstLine;
        if (_activeFirstLine >= _active->lines.size()) {
            _effectiveBytes -= _activeBytes;
            _active.reset();
            _activeFirstLine = 0;
            _activeBytes = 0;
        }
    }
    if (evicted) {
        _cellCount -= oldestCellCount;
        --_lineCount;
        ++_evictedLines;
        ++_version;
    }
}

void ChunkedScrollback::enforceLimits()
{
    while (_lineCount > 0
           && (_lineCount > _maxLines || _effectiveBytes > _maxBytes)) {
        evictOldest();
    }
}

bool ChunkedScrollback::popOldest(LogicalLine& line)
{
    const LogicalLine* source = lineAt(0);
    if (!source)
        return false;
    line = *source;
    evictOldest();
    return true;
}

void ChunkedScrollback::clear()
{
    for (const StoredChunk& stored : _chunks)
        _retired.push_back({stored.chunk, stored.chunk->byteSize});
    _chunks.clear();
    _active.reset();
    _activeFirstLine = 0;
    _activeBytes = 0;
    _lineCount = 0;
    _cellCount = 0;
    _effectiveBytes = 0;
    ++_version;
}

void ChunkedScrollback::setLimits(qsizetype maxLines, qsizetype maxBytes)
{
    _maxLines = std::clamp<qsizetype>(maxLines, 0, MaximumMaxLines);
    _maxBytes = std::max<qsizetype>(0, maxBytes);
    enforceLimits();
}

ScrollbackSnapshot ChunkedScrollback::snapshot()
{
    // Publishing the tail makes the Snapshot entirely immutable without an
    // unaccounted deep copy. Publication does not change document contents or
    // its version.
    publish();
    ScrollbackSnapshot result;
    result._version = _version;
    result._lineCount = _lineCount;
    result._chunks.reserve(qsizetype(_chunks.size()));
    qsizetype documentStart = 0;
    for (const StoredChunk& stored : _chunks) {
        const qsizetype count = stored.chunk->lines.size() - stored.firstLine;
        if (count <= 0)
            continue;
        result._chunks.push_back(
            {stored.chunk, stored.firstLine, count, documentStart});
        documentStart += count;
    }
    if (_lineCount > 0) {
        result._firstLineId = result.lineAt(0)->id;
        result._lastLineId = result.lineAt(_lineCount - 1)->id;
    }
    return result;
}

const LogicalLine* ChunkedScrollback::lineAt(qsizetype index) const
{
    if (index < 0 || index >= _lineCount)
        return nullptr;
    for (const StoredChunk& stored : _chunks) {
        const qsizetype count = stored.chunk->lines.size() - stored.firstLine;
        if (index < count)
            return &stored.chunk->lines[stored.firstLine + index];
        index -= count;
    }
    if (_active && index < _active->lines.size() - _activeFirstLine)
        return &_active->lines[_activeFirstLine + index];
    return nullptr;
}

void ChunkedScrollback::collectRetired() const
{
    _retired.erase(
        std::remove_if(_retired.begin(), _retired.end(),
                       [](const RetiredChunk& item) {
                           return item.chunk.expired();
                       }),
        _retired.end());
}

ScrollbackStatistics ChunkedScrollback::statistics() const
{
    collectRetired();
    ScrollbackStatistics result;
    result.version = _version;
    result.logicalLines = _lineCount;
    result.logicalCells = _cellCount;
    result.effectiveBytes = _effectiveBytes;
    result.sealedChunks = qsizetype(_chunks.size());
    result.activeLines = _active
        ? _active->lines.size() - _activeFirstLine : 0;
    result.evictedLines = _evictedLines;
    result.evictedChunks = _evictedChunks;
    for (const RetiredChunk& item : _retired) {
        if (!item.chunk.expired())
            result.retainedBySnapshots += item.bytes;
    }
    return result;
}

} // namespace NovaTerm
