/**
 * @file   RowBlockDamageTracker.h
 * @brief  按 8 列分块的行内容对账器：用真实快照校正调度层给出的脏区。
 *
 * RenderScheduler 送来的 DirtyRegion 只是**调度提示**，不保证覆盖本次
 * 解析批次里所有实际变化的列。典型反例是光标定位式重写（ConPTY 下的
 * PowerShell / Clink 尤其明显）：同一批次里先滚动、再擦除、再写入较短
 * 的一行，damage 矩形却只标出新写入的那几列。若照此增量重建，上一次
 * 较长行的行尾命令就会残留在屏幕上。
 *
 * 本类为每行缓存逐块（8 列一块）的内容哈希，`reconcileRow()` 拿最终
 * 快照的 Cell 重新计算并与缓存比对，把调度层漏掉的块补进脏列 span，
 * 而不是保守地整行重建。回归测试见 RendererP5Tests.cpp 的
 * `rowBlockDamageFindsOmittedStaleTail()`。
 *
 * @note 无锁，仅供 Renderer 在 GUI 线程内使用；不持有 QRhi 资源。
 */
#pragma once

#include "RenderCommandBuffer.h"
#include "core/terminal/ScreenBuffer.h"
#include "core/terminal/TerminalTypes.h"

#include <QVector>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace NovaTerm {

// 逐行、逐块记录 Renderer 当前已生成命令所对应的内容哈希。
class RowBlockDamageTracker
{
public:
    static constexpr int BlockColumns =
        RendererSnapshot::IdentityBlockColumns; ///< 每个脏块覆盖的列数

    /**
     * @brief 重置为新的视口尺寸，并把所有行标记为无缓存。
     * @param rows 可见行数。
     * @param columns 可见列数。
     * @note 必须维持的不变量：缓存的 rows/columns 与行槽位映射始终和当前
     *       视口一致。因此视口尺寸变化，以及任何重建行槽位映射的路径
     *       （`TerminalRenderer::resetWidgetRowMapping()`）都要调用本函数；
     *       否则会拿旧映射下的哈希与新快照比对，把已变化的块误判为"未变"
     *       而漏绘。
     */
    void reset(int rows, int columns)
    {
        _columns = std::max(0, columns);
        const int blockCount = (_columns + BlockColumns - 1) / BlockColumns;
        _rowHashes.resize(std::max(0, rows));
        _validRows.fill(false, _rowHashes.size());
        for (auto& hashes : _rowHashes)
            hashes.fill(0, blockCount);
    }

    /**
     * @brief 活动屏幕上滚 count 行时同步旋转缓存，保持行与哈希的对应。
     * @param count 上滚行数，超出行数时被夹紧。
     * @note 新进入视口的底部 count 行没有对应缓存，必须置为无效，
     *       否则它们会命中上一轮同槽位的哈希而被跳过重建。
     */
    void rotateRowsUp(int count)
    {
        if (_rowHashes.isEmpty())
            return;
        count = std::clamp(count, 0, int(_rowHashes.size()));
        if (count == 0)
            return;
        std::rotate(_rowHashes.begin(), _rowHashes.begin() + count,
                    _rowHashes.end());
        std::rotate(_validRows.begin(), _validRows.begin() + count,
                    _validRows.end());
        for (int row = int(_rowHashes.size()) - count;
             row < _rowHashes.size(); ++row) {
            _validRows[row] = false;
        }
    }

    /**
     * @brief 用最终快照校正一行的脏列 span。
     * @param row 行号（widget 行）。
     * @param blockIdentities Core快照提供的逐块内容指纹。
     * @param columns 该行列数，必须与 reset() 时一致。
     * @param requestedSpans 调度层给出的脏列 span，可为空。
     * @return 合并、排序、裁剪后的脏列 span；调用方据此只重建这些块。
     * @note 无法证明增量集合完整时保守返回整行 `[0, columns)`：行号越界、
     *       列数与缓存不符或 cells 为空都走这条路径。
     */
    QVector<DirtyColumnSpan> reconcileRow(
        int row, const std::vector<u64>& blockIdentities, int columns,
        QVector<DirtyColumnSpan> requestedSpans)
    {
        columns = std::max(0, columns);
        const int blockCount = (columns + BlockColumns - 1) / BlockColumns;
        if (row < 0 || row >= _rowHashes.size() || columns != _columns
            || isize(blockIdentities.size()) != blockCount) {
            return columns > 0
                ? QVector<DirtyColumnSpan>{{0, columns}}
                : QVector<DirtyColumnSpan>{};
        }

        auto& cached = _rowHashes[row];
        if (cached.size() != blockCount) {
            cached.fill(0, blockCount);
            _validRows[row] = false;
        }

        for (int block = 0; block < blockCount; ++block) {
            const int start = block * BlockColumns;
            const int end = std::min(columns, start + BlockColumns);
            const u64 current = blockIdentities[std::size_t(block)];
            if (!_validRows[row] || cached[block] != current)
                requestedSpans.push_back({start, end});
            cached[block] = current;
        }
        _validRows[row] = true;
        return mergeSpans(std::move(requestedSpans), columns);
    }

private:
    // 裁剪到 [0, columns)、丢弃空 span、按起始列排序后合并相邻或重叠区间。
    static QVector<DirtyColumnSpan> mergeSpans(
        QVector<DirtyColumnSpan> spans, int columns)
    {
        for (auto& span : spans) {
            span.startColumn = std::clamp(span.startColumn, 0, columns);
            span.endColumn = std::clamp(span.endColumn,
                                        span.startColumn, columns);
        }
        spans.erase(std::remove_if(spans.begin(), spans.end(),
                                   [](const DirtyColumnSpan& span) {
                                       return span.startColumn >= span.endColumn;
                                   }),
                    spans.end());
        std::sort(spans.begin(), spans.end(),
                  [](const DirtyColumnSpan& left,
                     const DirtyColumnSpan& right) {
                      return left.startColumn < right.startColumn;
                  });
        QVector<DirtyColumnSpan> merged;
        for (const auto& span : spans) {
            if (merged.isEmpty()
                || span.startColumn > merged.back().endColumn) {
                merged.push_back(span);
            } else {
                merged.back().endColumn = std::max(
                    merged.back().endColumn, span.endColumn);
            }
        }
        return merged;
    }

    int _columns{0};                        ///< reset() 时的列数
    QVector<QVector<quint64>> _rowHashes;   ///< [行][块] 内容哈希
    QVector<quint8> _validRows;             ///< 该行哈希是否可信
};

} // namespace NovaTerm
