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

#include <QVector>

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
        qsizetype firstLine{0};
        qsizetype lineCount{0};
        qsizetype documentStart{0};
    };

    quint64 version() const { return _version; }
    qsizetype lineCount() const { return _lineCount; }
    LineId firstLineId() const { return _firstLineId; }
    LineId lastLineId() const { return _lastLineId; }
    bool empty() const { return _lineCount == 0; }

    /**
     * @brief 按文档行号（0-based，从最旧行起算）查询逻辑行。
     * @param documentRow 文档行号，越界返回 nullptr。
     * @return 逻辑行指针，未命中返回 nullptr。
     */
    const LogicalLine* lineAt(qsizetype documentRow) const;

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
    qsizetype rowForLineId(LineId id) const;
    bool contains(LineId id) const { return rowForLineId(id) >= 0; }
    const QVector<ChunkView>& chunks() const { return _chunks; }

private:
    // 仅 ChunkedScrollback 在构建快照时可写。
    friend class ChunkedScrollback;
    QVector<ChunkView> _chunks;
    quint64 _version{0};
    qsizetype _lineCount{0};
    LineId _firstLineId{0};
    LineId _lastLineId{0};
};

} // namespace NovaTerm
