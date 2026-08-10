/**
 * @file   RowSlotMap.cpp
 * @brief  可见行 ↔ GPU 槽位映射实现。
 *
 * 详见 RowSlotMap.h。update() 是核心：建立 oldSlots 哈希后逐行查复用，
 * 未复用的行从 freeSlots 取槽位；rotatedRowsUp() 用 std::rotate 实现
 * 原地滚动。
 */
#include "RowSlotMap.h"

#include <algorithm>

namespace NovaTerm {

QVector<int> rowsNeedingRebuildAfterMapping(
    const QVector<quint64>& cachedIdentities,
    const QVector<quint64>& currentIdentities,
    const QVector<bool>& dirtyRows)
{
    QVector<int> result;
    result.reserve(currentIdentities.size());
    for (int row = 0; row < currentIdentities.size(); ++row) {
        // 已脏的行调用方会单独处理；identity 未变的行无需重建。
        if (dirtyRows.value(row)
            || (row < cachedIdentities.size()
                && cachedIdentities[row] == currentIdentities[row])) {
            continue;
        }
        result.push_back(row);
    }
    return result;
}

int RowSlotMap::allocateSlot(QVector<int>& freeSlots)
{
    if (!freeSlots.isEmpty())
        return freeSlots.takeLast();
    return _capacity++;
}

RowSlotUpdate RowSlotMap::update(const QVector<VisibleRowIdentity>& rows,
                                 float rowHeight, bool forceFull)
{
    RowSlotUpdate result;
    // 全量重映射条件：强制、首次映射、或行数变化。
    result.fullRemap = forceFull || _placements.isEmpty()
        || rows.size() != _placements.size();
    ++_mappingRevision;

    QHash<VisibleRowIdentity, int> oldSlots;
    QVector<int> freeSlots;
    if (!result.fullRemap) {
        // 增量路径：建 identity → gpuSlot 哈希，便于按 identity 查复用。
        for (const RowPlacement& placement : std::as_const(_placements))
            oldSlots.insert(placement.identity, placement.gpuSlot);
    } else {
        // 全量路径：所有旧槽位都退役。
        for (const RowPlacement& placement : std::as_const(_placements))
            result.retiredSlots.push_back(placement.gpuSlot);
    }

    // 标记仍被复用的槽位，未复用的进入 freeSlots 供新行分配。
    QVector<bool> retained(_capacity, false);
    if (!result.fullRemap) {
        for (const VisibleRowIdentity& identity : rows) {
            const auto found = oldSlots.constFind(identity);
            if (found != oldSlots.constEnd() && *found >= 0
                && *found < retained.size()) {
                retained[*found] = true;
            }
        }
        for (int slot = 0; slot < retained.size(); ++slot) {
            if (!retained[slot]) {
                freeSlots.push_back(slot);
                result.retiredSlots.push_back(slot);
            }
        }
    } else {
        for (int slot = 0; slot < _capacity; ++slot)
            freeSlots.push_back(slot);
    }

    result.placements.reserve(rows.size());
    for (int widgetRow = 0; widgetRow < rows.size(); ++widgetRow) {
        int slot = -1;
        bool reused = false;
        if (!result.fullRemap) {
            const auto found = oldSlots.constFind(rows[widgetRow]);
            if (found != oldSlots.constEnd()) {
                slot = *found;
                reused = true;
            }
        }
        if (slot < 0) {
            // 新进入的行：分配槽位并记录，调用方需上传顶点。
            slot = allocateSlot(freeSlots);
            result.enteringWidgetRows.push_back(widgetRow);
        } else {
            ++result.reusedRows;
        }
        result.placements.push_back({rows[widgetRow], widgetRow, slot,
                                     widgetRow * rowHeight,
                                     _mappingRevision, reused});
    }
    _placements = result.placements;
    return result;
}

void RowSlotMap::resetSequential(int rows, float rowHeight)
{
    rows = std::max(0, rows);
    ++_mappingRevision;
    _capacity = rows;
    _placements.resize(rows);
    for (int widgetRow = 0; widgetRow < rows; ++widgetRow) {
        RowPlacement& placement = _placements[widgetRow];
        placement.identity = {};
        placement.widgetRow = widgetRow;
        placement.gpuSlot = widgetRow;
        placement.yTransform = widgetRow * rowHeight;
        placement.mappingRevision = _mappingRevision;
        placement.reused = false;
    }
}

void RowSlotMap::rotateRowsUp(int count, float rowHeight)
{
    if (_placements.isEmpty())
        return;
    count = qBound(0, count, _placements.size());
    if (count == 0)
        return;

    // std::rotate 把 [begin, begin+count) 移到末尾，等效于"整体上移 count 行"。
    // 槽位本身不动，只调整 widgetRow 与 yTransform，避免顶点重传。
    std::rotate(_placements.begin(), _placements.begin() + count,
                _placements.end());
    ++_mappingRevision;
    for (int widgetRow = 0; widgetRow < _placements.size(); ++widgetRow) {
        RowPlacement& placement = _placements[widgetRow];
        placement.widgetRow = widgetRow;
        placement.yTransform = widgetRow * rowHeight;
        placement.mappingRevision = _mappingRevision;
        // 末尾 count 行是滚动后"新出现"的位置，内容需重新上传。
        placement.reused = widgetRow < _placements.size() - count;
    }
}

int RowSlotMap::slotForWidgetRow(int widgetRow) const
{
    if (widgetRow < 0 || widgetRow >= _placements.size())
        return -1;
    return _placements[widgetRow].gpuSlot;
}

bool RowSlotMap::isValidPermutation(int rows) const
{
    rows = std::max(0, rows);
    if (_placements.size() != rows || _capacity != rows)
        return false;

    // 校验：每行 widgetRow 与索引一致，gpuSlot 在 [0,rows) 且不重复。
    QVector<bool> seen(rows, false);
    for (int widgetRow = 0; widgetRow < rows; ++widgetRow) {
        const RowPlacement& placement = _placements[widgetRow];
        if (placement.widgetRow != widgetRow || placement.gpuSlot < 0
            || placement.gpuSlot >= rows || seen[placement.gpuSlot]) {
            return false;
        }
        seen[placement.gpuSlot] = true;
    }
    return true;
}

void RowSlotMap::reset()
{
    _placements.clear();
    _capacity = 0;
    ++_mappingRevision;
}

} // namespace NovaTerm
