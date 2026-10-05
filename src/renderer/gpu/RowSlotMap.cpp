/**
 * @file   RowSlotMap.cpp
 * @brief  可见行 ↔ GPU 槽位映射实现。
 *
 * 详见 RowSlotMap.h。本文件实现固定槽位环的两种映射方式：
 * `resetSequential()` 建立顺序排列，`rotateRowsUp()` 用 std::rotate 实现
 * 原地滚动（槽位本身不动，只调整 widgetRow 与 yTransform，避免顶点重传）。
 */
#include "RowSlotMap.h"

#include <algorithm>

namespace NovaTerm {

void RowSlotMap::resetSequential(int rows, float rowHeight)
{
    rows = std::max(0, rows);
    _capacity = rows;
    _placements.resize(rows);
    for (int widgetRow = 0; widgetRow < rows; ++widgetRow) {
        RowPlacement& placement = _placements[widgetRow];
        placement.widgetRow = widgetRow;
        placement.gpuSlot = widgetRow;
        placement.yTransform = widgetRow * rowHeight;
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
    std::rotate(_placements.begin(), _placements.begin() + count,
                _placements.end());
    for (int widgetRow = 0; widgetRow < _placements.size(); ++widgetRow) {
        RowPlacement& placement = _placements[widgetRow];
        placement.widgetRow = widgetRow;
        placement.yTransform = widgetRow * rowHeight;
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
    // 去重表是成员 scratch：isValidPermutation() 每个渲染帧都会被调用一次，
    // 局部 QVector<bool> 会在渲染热路径上每次分配一次。
    _seenSlots.resize(rows);
    std::fill(_seenSlots.begin(), _seenSlots.end(), false);
    for (int widgetRow = 0; widgetRow < rows; ++widgetRow) {
        const RowPlacement& placement = _placements[widgetRow];
        if (placement.widgetRow != widgetRow || placement.gpuSlot < 0
            || placement.gpuSlot >= rows || _seenSlots[placement.gpuSlot]) {
            return false;
        }
        _seenSlots[placement.gpuSlot] = true;
    }
    return true;
}

} // namespace NovaTerm