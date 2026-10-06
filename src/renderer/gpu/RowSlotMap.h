/**
 * @file   RowSlotMap.h
 * @brief  可见行 ↔ GPU 槽位映射。
 *
 * 固定行槽位环：可见第 i 行对应 GPU 缓冲第 `gpuSlot` 个槽位，槽位随行一起
 * 旋转（`rotateRowsUp`）或整体重排（`resetSequential`）。着色器用
 * `instanceMeta.y`（槽位号）索引 uniform 里的 `rowPlacement[]` 得到 y 偏移，
 * 因此**行内容留在原槽位、只改映射**就能整屏滚动而不重传顶点。
 *
 * @note 本类只提供两种映射方式，都不依赖行身份哈希：
 *  - `resetSequential()`：i → i，用于初始化与视口尺寸变化；
 *  - `rotateRowsUp()`：整体上移 count 行，用于活动屏幕上滚。
 *  基于 identity 哈希做增量槽位复用的 `update()` 已随渲染器的
 *  "live-scroll 行槽位旋转快路径" 一并删除（见 TerminalRenderer.cpp
 *  render() 内的说明）：该快路径在生产配置下永远不可达，保留它只会让
 *  行身份哈希与 retiredSlots/reusedRows 这些从不被读取的字段成为负担。
 */
#pragma once

#include <QVector>
#include <QtGlobal>

namespace NovaTerm {

// 单行的映射结果：widgetRow + gpuSlot + yTransform。
struct RowPlacement
{
    int widgetRow{-1};        // 屏幕行号（0=最上方可见行）
    int gpuSlot{-1};          // GPU 缓冲槽位号
    float yTransform{0};      // 该行在 GPU 中的 y 偏移（像素）
};

// 可见行 ↔ GPU 槽位映射器。单线程使用。
class RowSlotMap
{
public:
    /**
     * @brief 重置为顺序映射：第 i 行 → 第 i 个槽位。
     *        用于初始化或视口尺寸变化后的全量重建。
     * @param rows      行数；<= 0 时清空映射。
     * @param rowHeight 单行像素高度，用于计算 yTransform。
     */
    void resetSequential(int rows, float rowHeight);

    /**
     * @brief 把所有 placements 向上滚动 count 行：顶部 count 行被丢弃，
     *        底部 count 行变为新行。
     * @param count     上移行数；<= 0 或超出范围时不做任何事。
     * @param rowHeight 单行像素高度，用于重算 yTransform。
     * @note  当前**没有生产调用方**：随 live-scroll 旋转快路径一起被摘掉
     *        （见文件头）。与被删的 update() 不同，它不携带行身份哈希，
     *        正是将来按 shell 能力位重新启用快路径时唯一需要的那块机制，
     *        故按"幸存原语"保留，并由 sequentialRowSlotsStayValidAcrossResize
     *        AndScroll / rowSlotRingReusesScrolledRows 两个用例守住排列不变
     *        式。RowBlockDamageTracker::rotateRowsUp() 同理暂时无调用方，
     *        但那个头文件不在本次改动范围内。
     */
    void rotateRowsUp(int count, float rowHeight);

    /**
     * @brief 校验当前映射是否为合法排列：每行恰好对应一个不重复的槽位。
     *        用于渲染帧前的兜底检查。
     * @param rows 期望行数。
     * @note  逻辑上是 const（不改映射），去重表用 mutable scratch：调用点在
     *        渲染热路径上，不能每次分配一张去重表。
     */
    [[nodiscard]] bool isValidPermutation(int rows) const;

    [[nodiscard]] int slotForWidgetRow(int widgetRow) const;
    [[nodiscard]] const QVector<RowPlacement>& placements() const
    {
        return _placements;
    }

private:
    int _capacity{0};            // 已分配过的最大槽位号 +1
    QVector<RowPlacement> _placements;
    mutable QVector<bool> _seenSlots;  ///< isValidPermutation 的复用去重表
};

} // namespace NovaTerm