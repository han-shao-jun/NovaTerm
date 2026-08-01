#include "RenderCommandBuffer.h"

#include <QtGlobal>

#include <utility>
#include <algorithm>

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
        row.atlasGeneration = 0;
        row.contentRevision = 0;
        row.dirtySpans.clear();
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
    QVector<RenderCommand> contents,
    quint64 atlasGeneration,
    quint64 contentRevision,
    QVector<DirtyColumnSpan> dirtySpans)
{
    if (index < 0 || index >= _rowCommands.size())
        return;

    RenderCommandRow& destination = _rowCommands[index];
    destination.backgrounds = std::move(backgrounds);
    destination.contents = std::move(contents);
    destination.revision = ++_revision;
    destination.atlasGeneration = atlasGeneration;
    destination.contentRevision = contentRevision;
    destination.dirtySpans = std::move(dirtySpans);
}

void RenderCommandBuffer::replaceOverlays(QVector<RenderCommand> overlays)
{
    _overlays = std::move(overlays);
    ++_revision;
}

void RenderCommandBuffer::rotateRowsUp(int count)
{
    if (_rowCommands.isEmpty())
        return;
    count = qBound(0, count, _rowCommands.size());
    if (count == 0)
        return;
    std::rotate(_rowCommands.begin(), _rowCommands.begin() + count,
                _rowCommands.end());
    for (int row = _rowCommands.size() - count;
         row < _rowCommands.size(); ++row) {
        _rowCommands[row] = RenderCommandRow{};
        _rowCommands[row].revision = ++_revision;
    }
}

qsizetype RenderCommandBuffer::commandCount() const
{
    qsizetype count = _overlays.size();
    for (const RenderCommandRow& row : _rowCommands)
        count += row.backgrounds.size() + row.contents.size();
    return count;
}

bool RenderCommandBuffer::rowsUseAtlasGeneration(
    quint64 atlasGeneration) const
{
    for (const RenderCommandRow& row : _rowCommands) {
        if (row.atlasGeneration != atlasGeneration)
            return false;
    }
    return true;
}

} // namespace NovaTerm
