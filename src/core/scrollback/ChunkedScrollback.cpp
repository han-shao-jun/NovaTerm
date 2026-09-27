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
    // 顺手回收已无快照引用的退休块。collectRetired 原本只被 statistics()
    // 调用而生产路径无人调用它，_retired 会随会话时长无界增长（每块
    // ~64 B 的 weak_ptr + 控制块滞留）。retireChunk 本身是块粒度冷路径，
    // erase-remove 在列表保持短小的前提下代价可忽略。
    collectRetired();
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

void ChunkedScrollback::tailFrom(LineId sinceId, isize maxLines,
                                 ScrollbackTail& out) const
{
    out = ScrollbackTail{};
    out.version = _version;
    out.lineCount = _lineCount;
    if (_lineCount == 0)
        return;  // 空历史：first/last/from=0、lines 空、resync=false

    const LogicalLine* firstLine = lineAt(0);
    const LogicalLine* lastLine = lineAt(_lineCount - 1);
    if (!firstLine || !lastLine) {  // 理论不可达，防御性 resync
        out.resync = true;
        return;
    }
    out.firstLineId = firstLine->id;
    out.lastLineId = lastLine->id;

    // sinceId 那行已被头部淘汰（或调用方无有效锚点 sinceId==0）：历史剧变，
    // 交给全量重排。
    if (sinceId == 0 || sinceId < out.firstLineId) {
        out.resync = true;
        return;
    }
    // sinceId 被 sb_popline 取回（比现存最新还新）时，从现存最新行起收集：
    // 调用方会删掉被取回行的显示行并重折现存尾行（可能已被截断改写）。
    const LineId effectiveSince = std::min(sinceId, out.lastLineId);

    // 先往回定位 effectiveSince 的行号（只读 id、不拷贝），并施加 maxLines 上限。
    // 只依赖 lineAt 的 id 随 documentRow 单调升序，不依赖 id 连续无空洞。
    const isize maxCollect = std::max<isize>(1, maxLines);
    isize startRow = _lineCount;
    isize scanned = 0;
    for (isize row = _lineCount - 1; row >= 0; --row) {
        const LogicalLine* line = lineAt(row);
        if (!line || line->id < effectiveSince)
            break;  // 越过起点（id 空洞时的兜底）
        startRow = row;
        if (++scanned > maxCollect) {  // 落后过远：交给全量重排（worker 共享 cells）
            out.resync = true;
            return;
        }
        if (line->id == effectiveSince)
            break;
    }
    if (startRow >= _lineCount) {  // 未定位到（effectiveSince<=last 时理论必命中）
        out.resync = true;
        return;
    }

    // 正向深拷贝 [startRow .. lineCount)。深拷贝的是尾部少量逻辑行的 cells，
    // 成本远小于全量快照复制所有 ChunkView。
    out.lines.reserve(std::size_t(_lineCount - startRow));
    for (isize row = startRow; row < _lineCount; ++row) {
        const LogicalLine* line = lineAt(row);
        if (!line)
            break;
        out.lines.push_back(*line);
    }
    out.fromLineId = out.lines.empty() ? 0 : out.lines.front().id;
}

const LogicalLine* ChunkedScrollback::lineAt(isize index) const
{
    if (index < 0 || index >= _lineCount)
        return nullptr;
    // 从较近的一端扫描分块：tailFrom（每批 updateHistoryLayout 跑）与
    // snapshot() 的 lineAt(_lineCount-1) 都访问尾部，从头扫是 O(chunks)（大
    // scrollback 下约千块）。按 index 落在前/后半选择扫描方向，尾部访问接近
    // O(1)，最坏仍是 O(chunks/2)。active 块逻辑上位于所有 sealed 块之后。
    const isize activeLines =
        _active ? isize(_active->lines.size()) - _activeFirstLine : 0;
    if (index >= _lineCount - activeLines)
        return &_active->lines[_activeFirstLine + (index - (_lineCount - activeLines))];

    if (index <= (_lineCount - activeLines) / 2) {
        // 前半：从最旧分块正向累加。
        for (const StoredChunk& stored : _chunks) {
            if (index < stored.lineCount)
                return &stored.chunk->lines[stored.firstLine + index];
            index -= stored.lineCount;
        }
    } else {
        // 后半：从最新分块反向累加，remaining 为「index 到 sealed 尾的距离」。
        isize remaining = (_lineCount - activeLines) - index;
        for (auto it = _chunks.rbegin(); it != _chunks.rend(); ++it) {
            if (remaining <= it->lineCount)
                return &it->chunk->lines[it->firstLine + it->lineCount - remaining];
            remaining -= it->lineCount;
        }
    }
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
