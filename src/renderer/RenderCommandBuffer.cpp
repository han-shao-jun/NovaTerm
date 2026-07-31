#include "RenderCommandBuffer.h"

#include <QtGlobal>

#include <utility>

namespace NovaTerm {

void RenderCommandBuffer::resize(int rows, int columns)
{
    rows = qMax(0, rows);
    columns = qMax(0, columns);
    if (_rows == rows && _columns == columns)
        return;

    _rows = rows;
    _columns = columns;
    _rowCommands.resize(rows);
    for (RenderCommandRow& row : _rowCommands) {
        row.backgrounds.clear();
        row.contents.clear();
        row.revision = ++_revision;
    }
    _overlays.clear();
    ++_revision;
}

const RenderCommandRow& RenderCommandBuffer::row(int index) const
{
    Q_ASSERT(index >= 0 && index < _rowCommands.size());
    return _rowCommands[index];
}

void RenderCommandBuffer::replaceRow(
    int index,
    QVector<RenderCommand> backgrounds,
    QVector<RenderCommand> contents)
{
    if (index < 0 || index >= _rowCommands.size())
        return;

    RenderCommandRow& destination = _rowCommands[index];
    destination.backgrounds = std::move(backgrounds);
    destination.contents = std::move(contents);
    destination.revision = ++_revision;
}

void RenderCommandBuffer::replaceOverlays(QVector<RenderCommand> overlays)
{
    _overlays = std::move(overlays);
    ++_revision;
}

qsizetype RenderCommandBuffer::commandCount() const
{
    qsizetype count = _overlays.size();
    for (const RenderCommandRow& row : _rowCommands)
        count += row.backgrounds.size() + row.contents.size();
    return count;
}

} // namespace NovaTerm
