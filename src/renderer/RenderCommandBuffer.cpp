/**
 * @file   RenderCommandBuffer.cpp
 * @brief  渲染命令缓冲实现。
 *
 * 详见 RenderCommandBuffer.h。本文件维护按行组织的命令列表，提供
 * resize / mutableRow+finishRow 就地重建 / rotateRowsUp 等操作。
 */
#include "RenderCommandBuffer.h"

#include <QtGlobal>

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
    // resize 后所有行视为脏：清空命令并 bump revision 触发重绘。
    for (RenderCommandRow& row : _rowCommands) {
        row.backgrounds.clear();
        row.contents.clear();
        row.revision = ++_revision;
        row.atlasGeneration = 0;
        row.highlightRole = NoHighlightRole;
    }
    _overlays.clear();
    ++_revision;
}

const RenderCommandRow& RenderCommandBuffer::row(int index) const
{
    Q_ASSERT(index >= 0 && index < _rowCommands.size());
    return _rowCommands[index];
}

RenderCommandRow& RenderCommandBuffer::mutableRow(int index)
{
    if (index < 0 || index >= _rowCommands.size()) {
        // resize 竞态下重建方仍会写入：落点用固定 scratch，写完即丢。
        _scratchRow.backgrounds.clear();
        _scratchRow.contents.clear();
        // 角色也必须复位：重建方拿它与上一帧比较来决定是否整行替换，
        // 留着旧值会得到一个基于无关行的判据。
        _scratchRow.highlightRole = NoHighlightRole;
        return _scratchRow;
    }
    return _rowCommands[index];
}

void RenderCommandBuffer::finishRow(int index, quint64 atlasGeneration)
{
    if (index < 0 || index >= _rowCommands.size())
        return;
    RenderCommandRow& destination = _rowCommands[index];
    destination.revision = ++_revision;
    destination.atlasGeneration = atlasGeneration;
}

void RenderCommandBuffer::swapOverlays(QVector<RenderCommand>& scratch)
{
    _overlays.swap(scratch);
    ++_revision;
}

void RenderCommandBuffer::rotateRowsUp(int count)
{
    if (_rowCommands.isEmpty())
        return;
    count = qBound(0, count, _rowCommands.size());
    if (count == 0)
        return;
    // std::rotate 把 [begin, begin+count) 移到末尾，等效于"整体上移 count 行"。
    std::rotate(_rowCommands.begin(), _rowCommands.begin() + count,
                _rowCommands.end());
    // 末尾 count 行变为新空行，需要单独 bump revision 让渲染器重绘。
    // 只清空命令而保留向量容量：整行赋值会释放容量，滚动时每帧重新分配。
    for (int row = _rowCommands.size() - count;
         row < _rowCommands.size(); ++row) {
        RenderCommandRow& empty = _rowCommands[row];
        empty.backgrounds.clear();
        empty.contents.clear();
        empty.revision = ++_revision;
        empty.atlasGeneration = 0;
        empty.highlightRole = NoHighlightRole;
    }
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
