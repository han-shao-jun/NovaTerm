#pragma once

#include "TerminalTypes.h"

#include <QVector>

namespace NovaTerm {

class ScreenBuffer
{
public:
    ScreenBuffer(int columns = 80, int rows = 24);

    void resize(int columns, int rows);
    int columns() const { return _columns; }
    int rows() const { return _rows; }

    const Cell* cellAt(int row, int column) const;
    Cell* cellAt(int row, int column);
    void setCell(int row, int column, const Cell& cell);
    void moveRect(const DirtyRegion& destination, const DirtyRegion& source);
    void clear();

    const QVector<Cell>& cells() const { return _cells; }

private:
    int indexOf(int row, int column) const;

    int _columns{0};
    int _rows{0};
    QVector<Cell> _cells;
};

struct TerminalSnapshot
{
    int columns{0};
    int rows{0};
    QVector<Cell> visibleCells;
    CursorState cursor;

    const Cell* cellAt(int row, int column) const;
};

// Renderer-facing sparse snapshot. Only requested widget rows contain cells;
// active-screen and scrollback mapping is resolved while holding one model
// lock, so a frame cannot mix different history generations.
struct RendererSnapshot
{
    int columns{0};
    int rows{0};
    QVector<QVector<Cell>> visibleRows;
    CursorState cursor;

    const Cell* cellAt(int widgetRow, int column) const;
};

TerminalSnapshot makeSnapshot(const ScreenBuffer& screen,
                              const CursorState& cursor);

} // namespace NovaTerm
