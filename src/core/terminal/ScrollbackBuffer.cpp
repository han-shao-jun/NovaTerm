/**
 * @file   ScrollbackBuffer.cpp
 * @brief  滚动历史缓冲外观实现。
 *
 * 详见 ScrollbackBuffer.h 的接口说明。本文件将外部接口委托给
 * ChunkedScrollback 的对应方法，并维护一次性 _pendingLine 缓冲。
 */
#include "ScrollbackBuffer.h"

#include <algorithm>
#include <utility>

ScrollbackBuffer::ScrollbackBuffer(int maxLines)
    : _storage(std::max(0, maxLines),
               NovaTerm::ChunkedScrollback::DefaultMaxBytes)
    , _maxLines(std::max(0, maxLines))
{
}

void ScrollbackBuffer::pushLine(const NovaTerm::Cell* cells, int cols)
{
    if (_maxLines == 0 || !cells || cols <= 0)
        return;
    _cols = cols;
    _storage.append(cells, cols, true);
}

QVector<NovaTerm::Cell>& ScrollbackBuffer::beginPushLine(int columns,
                                                         int storedColumns)
{
    _cols = std::max(0, columns);
    // 截断或扩展 pending 缓冲到实际存储列数。
    _pendingLine.resize(std::clamp(storedColumns, 0, _cols));
    return _pendingLine;
}

void ScrollbackBuffer::commitPushLine(bool continuation, bool hardBreak)
{
    if (_maxLines > 0) {
        NovaTerm::LogicalLine line;
        line.cells = std::exchange(_pendingLine, {});
        line.hardBreak = hardBreak;
        if (continuation)
            _storage.appendContinuation(std::move(line));
        else
            _storage.append(std::move(line));
    } else {
        _pendingLine.clear();
    }
}

bool ScrollbackBuffer::popLine(NovaTerm::Cell* cells, int cols)
{
    NovaTerm::LogicalLine line;
    if (!_storage.popOldest(line))
        return false;
    const int count = std::min(cols, int(line.cells.size()));
    if (cells && count > 0)
        std::copy_n(line.cells.cbegin(), count, cells);
    return true;
}

void ScrollbackBuffer::clear()
{
    _storage.clear();
    _pendingLine.clear();
    _cols = 0;
}

void ScrollbackBuffer::setMaxLines(int max)
{
    _maxLines = std::clamp(max, 0,
        int(NovaTerm::ChunkedScrollback::MaximumMaxLines));
    _storage.setLimits(_maxLines,
                       NovaTerm::ChunkedScrollback::DefaultMaxBytes);
}

int ScrollbackBuffer::lineCount() const
{
    return int(_storage.lineCount());
}

const ScrollbackCell* ScrollbackBuffer::lineAt(int index) const
{
    const NovaTerm::LogicalLine* line = _storage.lineAt(index);
    return line ? line->cells.constData() : nullptr;
}

const QVector<ScrollbackCell>* ScrollbackBuffer::lineVectorAt(int index) const
{
    const NovaTerm::LogicalLine* line = _storage.lineAt(index);
    return line ? &line->cells : nullptr;
}

NovaTerm::ScrollbackSnapshot ScrollbackBuffer::snapshot()
{
    return _storage.snapshot();
}

NovaTerm::ScrollbackStatistics ScrollbackBuffer::statistics() const
{
    return _storage.statistics();
}
