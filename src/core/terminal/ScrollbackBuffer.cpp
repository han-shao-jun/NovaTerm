#include "ScrollbackBuffer.h"
#include <algorithm>
#include <utility>

ScrollbackBuffer::ScrollbackBuffer(int maxLines)
    : _maxLines(std::max(1, maxLines))
{
    _lines.resize(_maxLines);
}

void ScrollbackBuffer::pushLine(const NovaTerm::Cell* cells, int cols)
{
    if (_maxLines == 0 || cols <= 0)
        return;

    auto& line = beginPushLine(cols, cols);
    for (int c = 0; c < cols; ++c)
        line[c] = cells[c];
    commitPushLine();
}

QVector<NovaTerm::Cell>& ScrollbackBuffer::beginPushLine(int columns,
                                                         int storedColumns)
{
    _cols = columns;
    QVector<NovaTerm::Cell>& line = _lines[_writePos];
    line.resize(std::clamp(storedColumns, 0, columns));
    return line;
}

void ScrollbackBuffer::commitPushLine()
{
    _writePos = (_writePos + 1) % _maxLines;
    if (_count < _maxLines)
        ++_count;
}

bool ScrollbackBuffer::popLine(NovaTerm::Cell* cells, int cols)
{
    if (_count == 0)
        return false;

    // 从最旧的行弹出（环形缓冲头部）
    const int oldestPos = (_count < _maxLines)
        ? 0
        : _writePos;  // 环形满时，writePos 即是最旧位置
    const auto& line = _lines[oldestPos];

    const int copyCols = std::min(cols, static_cast<int>(line.size()));
    for (int c = 0; c < copyCols; ++c)
        cells[c] = line[c];

    --_count;
    // 非满状态时需整体前移（简化实现；scrollback 通常在满状态下工作）
    if (_count < _maxLines) {
        for (int i = 0; i < _count; ++i) {
            const int srcIdx = (oldestPos + 1 + i) % _maxLines;
            _lines[i].swap(_lines[srcIdx]);
        }
        _writePos = _count;
    }

    return true;
}

void ScrollbackBuffer::clear()
{
    _count = 0;
    _writePos = 0;
    _cols = 0;
}

void ScrollbackBuffer::setMaxLines(int max)
{
    max = std::max(1, max);
    if (max == _maxLines)
        return;

    QVector<QVector<ScrollbackCell>> newLines(max);
    if (_count > 0) {
        const int keep = std::min(_count, max);
        // 保留最新的 keep 行
        const int start = (_count >= _maxLines)
            ? (_writePos - _count + _maxLines) % _maxLines
            : 0;
        // 将最旧的 (_count - keep) 行丢弃，从相对旧的位置开始复制
        const int copyStart = (start + (_count - keep)) % _maxLines;
        for (int i = 0; i < keep; ++i) {
            const int srcIdx = (copyStart + i) % _maxLines;
            newLines[i].swap(_lines[srcIdx]);
        }
        _count = keep;
        _writePos = (keep == max) ? 0 : keep;
    } else {
        _writePos = 0;
    }

    _lines.swap(newLines);
    _maxLines = max;
}

int ScrollbackBuffer::lineCount() const
{
    return _count;
}

const ScrollbackCell* ScrollbackBuffer::lineAt(int index) const
{
    const auto* line = lineVectorAt(index);
    return line ? line->constData() : nullptr;
}

const QVector<ScrollbackCell>* ScrollbackBuffer::lineVectorAt(int index) const
{
    if (index < 0 || index >= _count)
        return nullptr;

    const int oldestPos = (_count < _maxLines) ? 0 : _writePos;
    const int realIdx = (oldestPos + index) % _maxLines;
    return &_lines[realIdx];
}
