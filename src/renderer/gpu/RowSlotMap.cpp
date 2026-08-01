#include "RowSlotMap.h"

#include <algorithm>

namespace NovaTerm {

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

void RowSlotMap::reset()
{
    _placements.clear();
    _capacity = 0;
    ++_mappingRevision;
}

} // namespace NovaTerm
