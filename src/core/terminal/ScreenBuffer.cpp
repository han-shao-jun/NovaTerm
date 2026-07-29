#include "ScreenBuffer.h"

#include <algorithm>

namespace NovaTerm {

ScreenBuffer::ScreenBuffer(int columns, int rows)
{
    resize(columns, rows);
}

void ScreenBuffer::resize(int columns, int rows)
{
    columns = std::max(1, columns);
    rows = std::max(1, rows);

    QVector<Cell> resized(columns * rows);
    const int copyRows = std::min(_rows, rows);
    const int copyColumns = std::min(_columns, columns);
    for (int row = 0; row < copyRows; ++row) {
        for (int column = 0; column < copyColumns; ++column)
            resized[row * columns + column] = _cells[row * _columns + column];
    }

    _columns = columns;
    _rows = rows;
    _cells.swap(resized);
}

const Cell* ScreenBuffer::cellAt(int row, int column) const
{
    const int index = indexOf(row, column);
    return index >= 0 ? &_cells[index] : nullptr;
}

Cell* ScreenBuffer::cellAt(int row, int column)
{
    const int index = indexOf(row, column);
    return index >= 0 ? &_cells[index] : nullptr;
}

void ScreenBuffer::setCell(int row, int column, const Cell& cell)
{
    if (Cell* destination = cellAt(row, column))
        *destination = cell;
}

void ScreenBuffer::clear()
{
    std::fill(_cells.begin(), _cells.end(), Cell{});
}

int ScreenBuffer::indexOf(int row, int column) const
{
    if (row < 0 || row >= _rows || column < 0 || column >= _columns)
        return -1;
    return row * _columns + column;
}

const Cell* TerminalSnapshot::cellAt(int row, int column) const
{
    if (row < 0 || row >= rows || column < 0 || column >= columns)
        return nullptr;
    return &visibleCells[row * columns + column];
}

TerminalSnapshot makeSnapshot(const ScreenBuffer& screen,
                              const CursorState& cursor)
{
    TerminalSnapshot snapshot;
    snapshot.columns = screen.columns();
    snapshot.rows = screen.rows();
    snapshot.visibleCells = screen.cells();
    snapshot.cursor = cursor;
    return snapshot;
}

} // namespace NovaTerm
