/**
 * @file   LineLayout.h
 * @brief  逻辑行折行与历史重排（reflow）引擎。
 *
 * 终端宽度变化或滚动回看时，需要把可能跨越多个软换行片段的 LogicalLine
 * 按当前列宽重新折行为 DisplayLine。LineLayout::wrapLine 处理单行折行，
 * LineLayout::viewport 处理可见视口；ReflowEngine 在独立线程中对整个
 * 滚动历史执行完整 reflow，分批回报结果，支持代际取消。
 */
#pragma once

#include "ScrollbackSnapshot.h"

#include <QObject>

#include <string>

#include <vector>

#include <atomic>
#include <functional>
#include <memory>

namespace NovaTerm {

// 单条逻辑行参与折行的 Cell 上限。超长逻辑行（例如 cat 一个没有换行的大
// 文件会形成一条数百万格的逻辑行）折出的 DisplayLine 数量会撑爆内存，也让
// 取消检查点之间的耗时失控。超出部分被截断，不参与布局。
// 该上限对 wrapLine 的所有调用方一致生效 —— 包括每帧调用的 viewport 与
// worker 线程上的 ReflowEngine，避免两条路径行为不同。
inline constexpr isize MaxWrapCells = 4 * 1024 * 1024;

// 显示行：一个 LogicalLine 在某个折行位置上的可见切片。
// wrapIndex 表示该 DisplayLine 在其 LogicalLine 内的折行序号（0-based）。
struct DisplayLine
{
    LineId lineId{0};
    isize startCell{0};
    isize endCell{0};
    isize wrapIndex{0};
    bool hardBreak{false};
};

// 视口快照：一次性折行结果，用于渲染滚动回看时的可见区域。
struct ViewportSnapshot
{
    u64 sourceVersion{0};
    u64 generation{0};
    int columns{0};
    std::vector<DisplayLine> rows;
};

// 一批 reflow 结果。completed=true 表示 reflow 已结束；
// cancelled=true 表示被更高 generation 取代而提前终止。
struct ReflowBatch
{
    u64 sourceVersion{0};
    u64 generation{0};
    isize logicalStart{0};
    isize logicalProcessed{0};
    isize physicalRows{0};
    std::vector<DisplayLine> rows;
    bool completed{false};
    bool cancelled{false};
    std::string error;  // UTF-8
};

// 静态折行工具：所有方法无状态、可跨线程调用。
class LineLayout
{
public:
    /**
     * @brief 把一个逻辑行按指定列宽折行为若干 DisplayLine。
     * @param line 待折行的逻辑行。
     * @param columns 目标列宽。
     * @param cancelled 取消回调；返回 true 时立即返回空 vector。
     * @return 折行后的 DisplayLine 列表，最后一行保留原行的 hardBreak 标志。
     */
    static std::vector<DisplayLine> wrapLine(
        const LogicalLine& line, int columns,
        const std::function<bool()>& cancelled = {});

    /**
     * @brief 计算从某逻辑行开始的可见视口折行结果。
     * @param snapshot 滚动历史快照。
     * @param anchorLine 起始逻辑行 ID。
     * @param wrapOffset 起始行在折行结果中的偏移（用于精确还原滚动位置）。
     * @param columns 目标列宽。
     * @param rowCount 需要的可见行数。
     * @param trailingCache 额外预折行的尾部行数，避免滚动时频繁重算。
     * @param generation 透传给 ViewportSnapshot 的代际标记。
     * @return 视口快照，包含至多 rowCount + trailingCache 行。
     */
    static ViewportSnapshot viewport(const ScrollbackSnapshot& snapshot,
                                     LineId anchorLine, isize wrapOffset,
                                     int columns, isize rowCount,
                                     isize trailingCache = 32,
                                     u64 generation = 0);
};

// 历史重排引擎：在独立线程中对整个滚动历史执行完整 reflow，分批通过
// batchReady 信号回报。代际机制保证新请求可快速取消旧请求，避免
// 用户连续调整列宽时累积无用计算。仅可在 GUI 线程创建/销毁。
class ReflowEngine final : public QObject
{
    Q_OBJECT
public:
    explicit ReflowEngine(QObject* parent = nullptr);
    ~ReflowEngine() override;

    /**
     * @brief 提交一次完整 reflow 请求。
     * @param snapshot 待 reflow 的滚动历史快照。
     * @param columns 目标列宽。
     * @param generation 代际标记，调用方保证单调递增。
     * @param batchLines 单批最大逻辑行数。
     */
    void request(ScrollbackSnapshot snapshot, int columns,
                 u64 generation, isize batchLines = 1024);
    void cancel(u64 generation);

signals:
    void batchReady(const NovaTerm::ReflowBatch& batch);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::DisplayLine)
Q_DECLARE_METATYPE(NovaTerm::ViewportSnapshot)
Q_DECLARE_METATYPE(NovaTerm::ReflowBatch)
