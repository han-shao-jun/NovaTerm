/** @file ScrollbackSnapshot.cpp
 * @brief 只读快照：所有定位只读索引，行读取显式持有拥有型句柄。
 */
#include "ScrollbackSnapshot.h"
#include "ScrollbackChunk.h"
namespace NovaTerm {
namespace {
const ScrollbackSnapshot::ChunkView* viewAt(const std::vector<ScrollbackSnapshot::ChunkView>& chunks, isize row) {
    isize lo = 0, hi = isize(chunks.size());
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2; const auto& view = chunks[std::size_t(mid)];
        if (row < view.documentStart) hi = mid;
        else if (row >= view.documentStart + view.lineCount) lo = mid + 1;
        else return &view;
    }
    return nullptr;
}
}
bool ScrollbackSnapshot::requiresBackgroundRead() const {
    for (const auto& view : _chunks) if (view.chunk->encoded) return true;
    return false;
}
HistoryLineIndex ScrollbackSnapshot::lineInfoAt(isize row) const {
    if (row < 0 || row >= _lineCount) return {};
    const auto* view = viewAt(_chunks, row);
    return view ? chunkLineInfo(*view->chunk, view->firstLine + row - view->documentStart) : HistoryLineIndex{};
}
PinnedHistoryLine ScrollbackSnapshot::lineAt(isize row, HistoryReadMode mode) const {
    if (row < 0 || row >= _lineCount) return {};
    const auto* view = viewAt(_chunks, row);
    return view ? pinChunkLine(view->chunk, view->firstLine + row - view->documentStart, mode) : PinnedHistoryLine{};
}
PinnedHistoryLine ScrollbackSnapshot::lineById(LineId id, HistoryReadMode mode) const {
    return lineAt(rowForLineId(id), mode);
}
isize ScrollbackSnapshot::rowForLineId(LineId id) const {
    if (!_lineCount || id < _firstLineId || id > _lastLineId) return -1;
    isize lo = 0, hi = isize(_chunks.size());
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2; const auto& view = _chunks[std::size_t(mid)];
        if (id < chunkLineInfo(*view.chunk, view.firstLine).id) hi = mid;
        else if (id > chunkLineInfo(*view.chunk, view.firstLine + view.lineCount - 1).id) lo = mid + 1;
        else {
            isize first = view.firstLine, last = first + view.lineCount;
            while (first < last) {
                const auto middle = first + (last - first) / 2;
                if (chunkLineInfo(*view.chunk, middle).id < id) first = middle + 1; else last = middle;
            }
            return first < view.firstLine + view.lineCount && chunkLineInfo(*view.chunk, first).id == id
                ? view.documentStart + first - view.firstLine : -1;
        }
    }
    return -1;
}
PinnedHistoryLine HistoryReadCursor::lineAt(isize row) {
    if (_pin && row >= _first && row < _last)
        return {_pin._owner, _pin._line + row - _pinnedRow};
    _pin = {};
    if (row < 0 || row >= _snapshot.lineCount()) return {};
    const auto* view = viewAt(_snapshot._chunks, row);
    if (!view) return {};
    _first = view->documentStart; _last = _first + view->lineCount; _pinnedRow = row;
    _pin = _snapshot.lineAt(row, HistoryReadMode::Sequential);
    return _pin;
}
} // namespace NovaTerm
