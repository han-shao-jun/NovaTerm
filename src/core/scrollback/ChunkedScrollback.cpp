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
constexpr isize ChunkAllocationOverhead = 64;
}

ChunkedScrollback::ChunkedScrollback(isize maxLines, isize maxBytes,
                                     isize chunkLines)
    : _maxLines(std::clamp<isize>(maxLines, 0, MaximumMaxLines))
    , _maxBytes(std::max<isize>(0, maxBytes))
    , _chunkLines(std::max<isize>(1, chunkLines))
{
}

isize ChunkedScrollback::lineBytes(const LogicalLine& line)
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
    const isize bytes = lineBytes(line);
    _cellCount += line.cells.size();
    _lineCount++;
    _activeBytes += bytes;
    _effectiveBytes += bytes;
    _active->lines.push_back(std::move(line));
    ++_version;

    // active 块写满即封存，避免单块过大导致快照共享粒度粗糙。
    if (isize(_active->lines.size()) >= _chunkLines)
        sealActive();
    enforceLimits();
    return id;
}

// 追加软换行片段：与上一行拼接为同一逻辑行。
// makeNewestLineWritable() 已保证最新行位于 active 块，因此这里只有一条路径。
LineId ChunkedScrollback::appendContinuation(LogicalLine fragment)
{
    // 缓冲为空（或状态不一致）时退化为普通 append，避免静默丢失片段。
    if (_lineCount == 0 || !makeNewestLineWritable())
        return append(std::move(fragment));

    const isize addedCells = fragment.cells.size();
    LogicalLine& line = _active->lines.back();
    const isize before = lineBytes(line);
    line.cells.insert(line.cells.end(),
                      fragment.cells.begin(), fragment.cells.end());
    line.hardBreak = fragment.hardBreak;
    const isize delta = lineBytes(line) - before;
    _activeBytes += delta;
    _effectiveBytes += delta;
    const LineId id = line.id;

    _cellCount += addedCells;
    ++_version;
    enforceLimits();
    return id;
}

LineId ChunkedScrollback::append(const Cell* cells, isize columns,
                                 bool hardBreak)
{
    LogicalLine line;
    line.hardBreak = hardBreak;
    if (cells && columns > 0)
        line.cells.assign(cells, cells + columns);
    return append(std::move(line));
}

// 封存 active 块：移入 _chunks 并清空 _active。_activeFirstLine 用于
// 跳过已被 evictOldest 淘汰但仍占用 lines 容器头部的行。
void ChunkedScrollback::sealActive()
{
    if (!_active || _activeFirstLine >= isize(_active->lines.size())) {
        // 无有效行：整块丢弃，其固定开销也要从有效字节中扣除。
        _effectiveBytes -= _activeBytes;
        _active.reset();
        _activeFirstLine = 0;
        _activeBytes = 0;
        return;
    }
    // active 头部已被淘汰的行仍占用 lines 数组，封存时需把它们的
    // 字节从分块有效字节数中扣除，避免统计虚高。
    const isize skippedBytes = [&]() {
        isize value = 0;
        for (isize i = 0; i < _activeFirstLine; ++i)
            value += lineBytes(_active->lines[i]);
        return value;
    }();
    const isize firstLine = _activeFirstLine;
    ScrollbackChunkPtr sealed = sealChunk(std::move(_active));
    StoredChunk stored;
    stored.chunk = sealed;
    stored.firstLine = firstLine;
    stored.lineCount = isize(sealed->lines.size()) - firstLine;
    stored.effectiveBytes = isize(sealed->byteSize
                                      - isize(sizeof(ScrollbackChunk))
                                      - ChunkAllocationOverhead
                                      - skippedBytes);
    _chunks.push_back(std::move(stored));
    _activeFirstLine = 0;
    _activeBytes = 0;
}

// 把最新逻辑行搬进 active 块，使其可被原地改写。详见头文件注释。
bool ChunkedScrollback::makeNewestLineWritable()
{
    if (_active && _activeFirstLine < isize(_active->lines.size()))
        return true;
    if (_chunks.empty())
        return false;

    StoredChunk& stored = _chunks.back();
    if (stored.lineCount <= 0)
        return false;
    const LogicalLine& newest =
        stored.chunk->lines[stored.firstLine + stored.lineCount - 1];
    const isize movedBytes = lineBytes(newest);

    // 只复制这一行；ensureActive 可能新分配一个 active 块并计入固定开销。
    ensureActive();
    _active->lines.push_back(newest);
    _activeBytes += movedBytes;
    _effectiveBytes += movedBytes;

    // 封存分块的有效区间从尾部裁掉一行。该行的字节记入 detachedBytes，
    // 使整块退休时不会把已搬走的部分二次扣除。
    --stored.lineCount;
    stored.effectiveBytes -= movedBytes;
    stored.detachedBytes += movedBytes;
    _effectiveBytes -= movedBytes;
    if (stored.lineCount <= 0)
        retireChunk(stored, false);
    return true;
}

// 分块再无有效行：从 _chunks 摘除并交给 _retired，等旧快照释放后回收内存。
// 调用方须保证 stored 就是 _chunks 的首个或末个元素。
// countAsEvicted=false 用于"尾行被搬进 active"这种内容未丢失的情况，
// 避免虚增 evictedChunks 统计。
void ChunkedScrollback::retireChunk(StoredChunk& stored, bool countAsEvicted)
{
    _effectiveBytes -= stored.chunk->byteSize - stored.detachedBytes;
    _retired.push_back({stored.chunk, stored.chunk->byteSize});
    if (&stored == &_chunks.front())
        _chunks.pop_front();
    else
        _chunks.pop_back();
    if (countAsEvicted)
        ++_evictedChunks;
}

void ChunkedScrollback::publish()
{
    if (_active && _activeFirstLine < isize(_active->lines.size()))
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
    isize oldestCellCount = 0;
    if (!_chunks.empty()) {
        StoredChunk& stored = _chunks.front();
        oldestCellCount = stored.chunk->lines[stored.firstLine].cells.size();
        evicted = true;
        ++stored.firstLine;
        --stored.lineCount;
        if (stored.lineCount <= 0)
            retireChunk(stored, true);
    } else if (_active && _activeFirstLine < isize(_active->lines.size())) {
        oldestCellCount = _active->lines[_activeFirstLine].cells.size();
        evicted = true;
        ++_activeFirstLine;
        if (_activeFirstLine >= isize(_active->lines.size())) {
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

namespace {

// 从 line 尾部取走至多 cellCount 个 Cell 到 out，返回实际取走的数量。
isize takeTailCells(LogicalLine& line, isize cellCount,
                        LogicalLine& out)
{
    const isize take = std::min(cellCount, isize(line.cells.size()));
    if (take <= 0)
        return 0;
    out.id = line.id;
    out.hardBreak = line.hardBreak;
    out.cells.assign(line.cells.end() - take, line.cells.end());
    line.cells.erase(line.cells.end() - take, line.cells.end());
    // 尾段回到了活动屏幕，剩余部分在历史中不再以硬换行结尾。
    line.hardBreak = false;
    return take;
}

} // namespace

bool ChunkedScrollback::takeNewestTail(isize cellCount, LogicalLine& out)
{
    if (_lineCount == 0 || cellCount <= 0)
        return false;
    // makeNewestLineWritable() 已保证最新行位于 active 块，可原地截断。
    if (!makeNewestLineWritable())
        return false;

    LogicalLine& line = _active->lines.back();
    const isize before = lineBytes(line);
    const isize taken = takeTailCells(line, cellCount, out);
    // taken == 0 意味着最新逻辑行本就是空行（cellCount > 0 已由上方保证，
    // takeTailCells 仅在行为空时取到 0）。空行代表输出中的一个空白行：
    // libvterm 变高回填（sb_popline）时应当作一个空白屏幕行取回，因此这里
    // 移除该空逻辑行并返回 true（out 携带原行 id/hardBreak，cells 为空，
    // 调用方回填空白）。**不能因 taken==0 返回 false** —— 那会让 sb_popline
    // 收到 0 而停止回填，导致空行以上的历史在窗口变高时无法拉回。
    if (taken == 0) {
        out.id = line.id;
        out.hardBreak = line.hardBreak;
    }
    _cellCount -= taken;
    if (line.cells.empty()) {
        _active->lines.pop_back();
        --_lineCount;
        _activeBytes -= before;
        _effectiveBytes -= before;
        if (_activeFirstLine >= isize(_active->lines.size())) {
            // active 已无有效行，整块释放。
            _effectiveBytes -= _activeBytes;
            _active.reset();
            _activeFirstLine = 0;
            _activeBytes = 0;
        }
    } else {
        const isize delta = lineBytes(line) - before;
        _activeBytes += delta;
        _effectiveBytes += delta;
    }
    ++_version;
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

void ChunkedScrollback::setLimits(isize maxLines, isize maxBytes)
{
    _maxLines = std::clamp<isize>(maxLines, 0, MaximumMaxLines);
    _maxBytes = std::max<isize>(0, maxBytes);
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
    result._chunks.reserve(isize(_chunks.size()));
    isize documentStart = 0;
    for (const StoredChunk& stored : _chunks) {
        if (stored.lineCount <= 0)
            continue;
        result._chunks.push_back({stored.chunk, stored.firstLine,
                                  stored.lineCount, documentStart});
        documentStart += stored.lineCount;
    }
    if (_lineCount > 0) {
        result._firstLineId = result.lineAt(0)->id;
        result._lastLineId = result.lineAt(_lineCount - 1)->id;
    }
    return result;
}

const LogicalLine* ChunkedScrollback::lineAt(isize index) const
{
    if (index < 0 || index >= _lineCount)
        return nullptr;
    // 顺序遍历分块；分块数量通常较少（默认 1024 行/块），顺序查找足够。
    for (const StoredChunk& stored : _chunks) {
        if (index < stored.lineCount)
            return &stored.chunk->lines[stored.firstLine + index];
        index -= stored.lineCount;
    }
    if (_active && index < isize(_active->lines.size()) - _activeFirstLine)
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
    result.sealedChunks = isize(_chunks.size());
    result.activeLines = _active
        ? isize(_active->lines.size()) - _activeFirstLine : 0;
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
