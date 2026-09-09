/**
 * @file   ScrollbackSnapshot.h
 * @brief  滚动历史只读快照。
 *
 * 快照持有 ChunkedScrollback 在某一时刻所有已封存分块的 shared_ptr 引用，
 * 因此即使 ChunkedScrollback 后续追加或淘汰分块，已发出的快照仍保持
 * 对应版本数据不变。查询接口支持按文档行号或按全局 LineId 两种坐标。
 */
#pragma once

#include "ScrollbackTypes.h"

#include <vector>

namespace NovaTerm {

class ChunkedScrollback;

// 滚动历史只读快照。不可变；多个快照可共享同一分块以降低内存占用。
class ScrollbackSnapshot
{
public:
    // 快照中一个分块的视图：记录该分块在文档中的起止行与
    // 在分块内部 lines 数组中的起始偏移。
    struct ChunkView
    {
        ScrollbackChunkPtr chunk;
        isize firstLine{0};
        isize lineCount{0};
        isize documentStart{0};
    };

    u64 version() const { return _version; }
    isize lineCount() const { return _lineCount; }
    LineId firstLineId() const { return _firstLineId; }
    LineId lastLineId() const { return _lastLineId; }
    bool empty() const { return _lineCount == 0; }

    /**
     * @brief 按文档行号（0-based，从最旧行起算）查询逻辑行。
     * @param documentRow 文档行号，越界返回 nullptr。
     * @return 逻辑行指针，未命中返回 nullptr。
     */
    const LogicalLine* lineAt(isize documentRow) const;

    /**
     * @brief 按全局 LineId 查询逻辑行。
     * @param id 全局行 ID。
     * @return 逻辑行指针，未命中返回 nullptr。
     */
    const LogicalLine* lineById(LineId id) const;

    /**
     * @brief 把全局 LineId 折算为文档行号。
     * @param id 全局行 ID。
     * @return 文档行号（0-based），未命中返回 -1。
     */
    isize rowForLineId(LineId id) const;
    bool contains(LineId id) const { return rowForLineId(id) >= 0; }
    const std::vector<ChunkView>& chunks() const { return _chunks; }

private:
    // 仅 ChunkedScrollback 在构建快照时可写。
    friend class ChunkedScrollback;
    std::vector<ChunkView> _chunks;
    u64 _version{0};
    isize _lineCount{0};
    LineId _firstLineId{0};
    LineId _lastLineId{0};
};

// 滚动历史的尾部增量视图。只携带增量维护显示布局所需的最小信息 ——
// 首行 ID（头部淘汰判据）、行数，以及尾部若干逻辑行的深拷贝。与
// ScrollbackSnapshot 不同，它**不封存 active 块、不复制每个分块的 ChunkView**，
// 因此可在每批输出后高频调用而不产生分块碎片化与 ChunkView churn。
// 服务于 TerminalRenderer::updateHistoryLayout 的增量路径。
struct ScrollbackTail
{
    u64 version{0};
    isize lineCount{0};
    LineId firstLineId{0};  // 当前最旧行 ID，用于删除已淘汰行的显示行
    LineId lastLineId{0};   // 当前最新行 ID
    // lines[0] 的 ID；lines 为空时为 0。调用方据此删除自己已有布局中
    // lineId >= fromLineId 的显示行，再对 lines 逐条重新折行。
    LineId fromLineId{0};
    // [fromLineId .. lastLineId] 的逻辑行深拷贝，按文档顺序（旧→新）排列。
    std::vector<LogicalLine> lines;
    // true 表示 sinceId 已被头部淘汰，或落后超过 maxLines —— 调用方应放弃
    // 增量、改走全量重排（scheduleReflow），本视图的 lines 不可用。
    bool resync{false};
};

} // namespace NovaTerm
