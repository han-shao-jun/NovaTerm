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
    result.fullRemap = forceFull || _placements.isEmpty()
        || rows.size() != _placements.size();
    ++_mappingRevision;

    QHash<VisibleRowIdentity, int> oldSlots;
    QVector<int> freeSlots;
    if (!result.fullRemap) {
        for (const RowPlacement& placement : std::as_const(_placements))
            oldSlots.insert(placement.identity, placement.gpuSlot);
    } else {
        for (const RowPlacement& placement : std::as_const(_placements))
            result.retiredSlots.push_back(placement.gpuSlot);
    }

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

    std::rotate(_placements.begin(), _placements.begin() + count,
                _placements.end());
    ++_mappingRevision;
    for (int widgetRow = 0; widgetRow < _placements.size(); ++widgetRow) {
        RowPlacement& placement = _placements[widgetRow];
        placement.widgetRow = widgetRow;
        placement.yTransform = widgetRow * rowHeight;
        placement.mappingRevision = _mappingRevision;
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
