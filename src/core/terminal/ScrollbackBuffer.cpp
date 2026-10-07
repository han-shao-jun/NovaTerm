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

// ScrollbackBuffer 处于全局命名空间，引入核心整数别名。
using NovaTerm::isize;

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

std::vector<NovaTerm::Cell>& ScrollbackBuffer::beginPushLine(int columns,
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

bool ScrollbackBuffer::popLine(NovaTerm::Cell* cells, int cols, bool* continuation)
{
    if (cols <= 0 || _storage.lineCount() == 0)
        return false;

    // libvterm 在屏幕变高时用 sb_popline 反向取回紧邻屏幕顶部的那一行 ——
    // 是**最新**的历史行。旧实现取的是最旧一行，且把整条逻辑行弹出后只回填
    // 前 cols 格，其余 Cell 被永久丢弃。
    const NovaTerm::LogicalLine* newest =
        _storage.lineAt(_storage.lineCount() - 1);
    if (!newest)
        return false;

    // 按目标 cols 重排该逻辑行：除末行外每段恰好 cols 格，因此末行长度
    // 为 total % cols，整除时说明末行也是满行。空行（total==0）取 cols，
    // takeNewestTail 会移除该空逻辑行并回填一整行空白 —— 不能在此因空行提前
    // 返回 false，否则 libvterm 收到 0 会停止回填，空行以上的历史无法拉回。
    const isize total = newest->cells.size();
    const isize remainder = total % cols;
    const isize rowCells = remainder == 0 ? isize(cols) : remainder;
    const bool hasPrefix = total > rowCells;

    NovaTerm::LogicalLine row;
    if (!_storage.takeNewestTail(rowCells, row))
        return false;
    if (continuation)
        *continuation = hasPrefix;

    if (cells) {
        const int count = std::min(cols, int(row.cells.size()));
        if (count > 0)
            std::copy_n(row.cells.cbegin(), count, cells);
        // 行尾补默认 Cell，避免调用方读到未初始化内容。
        for (int column = count; column < cols; ++column)
            cells[column] = NovaTerm::Cell{};
    }
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
    return line ? line->cells.data() : nullptr;
}

const std::vector<ScrollbackCell>* ScrollbackBuffer::lineVectorAt(int index) const
{
    const NovaTerm::LogicalLine* line = _storage.lineAt(index);
    return line ? &line->cells : nullptr;
}

NovaTerm::ScrollbackSnapshot ScrollbackBuffer::snapshot()
{
    return _storage.snapshot();
}

void ScrollbackBuffer::tailFrom(NovaTerm::LineId sinceId, isize maxLines,
                                NovaTerm::ScrollbackTail& out) const
{
    _storage.tailFrom(sinceId, maxLines, out);
}

NovaTerm::ScrollbackStatistics ScrollbackBuffer::statistics() const
{
    return _storage.statistics();
}
