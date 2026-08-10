/**
 * @file   ChunkedScrollback.cpp
 * @brief  分块滚动历史后端实现。
 *
 * 详见 ChunkedScrollback.h 的接口说明。本文件维护 active 块与已封存分块
 * 列表，按行数/字节上限淘汰最旧分块；为支持旧快照延迟释放，被淘汰分块
 * 通过 weak_ptr 暂存于 _retired，等所有快照释放后才真正回收内存。
 */
#include "ChunkedScrollback.h"

#include "ScrollbackChunk.h"

#include <algorithm>
#include <limits>

namespace NovaTerm {

namespace {
// 每个 ScrollbackChunk 的分配器/控制块固定开销估算。
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

// 确保 _active 已就绪：每次开始写入前调用，懒分配以避免空缓冲开销。
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
    // 行 ID 由本对象分配，不接受调用方传入的 ID。这保证跨淘汰的严格单调，
    // 是 ScrollbackSnapshot 二分查找的前提。
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

    // active 块写满即封存，避免单块过大导致快照共享粒度粗糙。
    if (_active->lines.size() >= _chunkLines)
        sealActive();
    enforceLimits();
    return id;
}

// 追加软换行片段：与上一行拼接为同一逻辑行。
// 三种情况：active 块中有上一行（最常见）、上一行在最后一个封存块
// （需 copy-on-write 替换）、或缓冲为空（退化为普通 append）。
LineId ChunkedScrollback::appendContinuation(LogicalLine fragment)
{
    if (_lineCount == 0)
        return append(std::move(fragment));
    const qsizetype addedCells = fragment.cells.size();
    LineId id = 0;
    if (_active && _activeFirstLine < _active->lines.size()) {
        // 情况 1：上一行在 active 块，直接拼接。
        LogicalLine& line = _active->lines.last();
        const qsizetype before = lineBytes(line);
        line.cells += fragment.cells;
        line.hardBreak = fragment.hardBreak;
        const qsizetype delta = lineBytes(line) - before;
        _activeBytes += delta;
        _effectiveBytes += delta;
        id = line.id;
    } else if (!_chunks.empty()) {
        // 情况 2：上一行在已封存块，必须 copy-on-write：复制整个分块、
        // 修改副本、重新封存，旧分块进入 _retired 等待旧快照释放。
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

// 封存 active 块：移入 _chunks 并清空 _active。_activeFirstLine 用于
// 跳过已被 evictOldest 淘汰但仍占用 lines 容器头部的行。
void ChunkedScrollback::sealActive()
{
    if (!_active || _activeFirstLine >= _active->lines.size()) {
        _active.reset();
        _activeFirstLine = 0;
        _activeBytes = 0;
        return;
    }
    // active 头部已被淘汰的行仍占用 lines 数组，封存时需把它们的
    // 字节从分块有效字节数中扣除，避免统计虚高。
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

// 淘汰最旧的一行。优先从最旧的封存块淘汰；若封存块列表为空
// （所有行都还在 active 块中），则从 active 头部淘汰。
// 分块整体被淘汰完时移入 _retired 等待旧快照释放。
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
    // 所有封存块进入 retired，等旧快照释放后才真正回收。
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
    // 先封存 active 尾块，使快照完全不可变且字节数已正确计入，
    // 无需任何深拷贝。封存只影响内存组织，不改变文档内容或版本号。
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
    // 顺序遍历分块；分块数量通常较少（默认 1024 行/块），顺序查找足够。
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

// 清理 _retired 中已无任何快照引用的分块，回收其统计计数。
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
    // 仍被旧快照持有的淘汰分块计入 retainedBySnapshots，便于排查内存
    // 无法回收的问题（通常是某个长生命周期快照未释放）。
    for (const RetiredChunk& item : _retired) {
        if (!item.chunk.expired())
            result.retainedBySnapshots += item.bytes;
    }
    return result;
}

} // namespace NovaTerm
