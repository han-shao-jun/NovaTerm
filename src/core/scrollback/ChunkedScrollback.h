/**
 * @file   ChunkedScrollback.h
 * @brief  分块滚动历史后端实现。
 *
 * 终端活动屏幕上滚出的行追加到本对象。为平衡追加吞吐与快照共享开销，
 * 行被分批打包为 ScrollbackChunk（默认 1024 行/块），active 块在
 * snapshot() 时封存为不可变 const 共享指针，多个快照可共享同一分块。
 * 当行数或字节数超过上限时，从最旧的分块开始淘汰。
 */
#pragma once

#include "ScrollbackSnapshot.h"

#include <deque>
#include <memory>
#include <vector>

namespace NovaTerm {

// 分块滚动历史后端。不可拷贝，单线程拥有（由 TerminalCore::Runtime 持有）。
class ChunkedScrollback
{
public:
    static constexpr qsizetype DefaultChunkLines = 1024;
    static constexpr qsizetype DefaultMaxLines = 100'000;
    static constexpr qsizetype MaximumMaxLines = 1'000'000;
    static constexpr qsizetype DefaultMaxBytes = 256 * 1024 * 1024;

    explicit ChunkedScrollback(qsizetype maxLines = DefaultMaxLines,
                               qsizetype maxBytes = DefaultMaxBytes,
                               qsizetype chunkLines = DefaultChunkLines);

    /**
     * @brief 追加一个独立逻辑行（hardBreak=true）。
     * @return 该行的全局 LineId。
     */
    LineId append(LogicalLine line);

    /**
     * @brief 追加一个软换行片段，与上一行拼接为同一逻辑行。
     * @return 拼接后逻辑行的 LineId。
     */
    LineId appendContinuation(LogicalLine fragment);

    /**
     * @brief 便捷重载：把一段 Cell 数组作为一个新行追加。
     * @param cells Cell 数组指针。
     * @param columns Cell 数量。
     * @param hardBreak 是否硬换行结尾。
     * @return 该行的 LineId。
     */
    LineId append(const Cell* cells, qsizetype columns,
                  bool hardBreak = true);

    /**
     * @brief 弹出最旧的一行（用于 libvterm 的 reverse index 回滚）。
     * @param line 输出参数，接收弹出的行。
     * @return true 表示成功弹出；false 表示缓冲为空。
     */
    bool popOldest(LogicalLine& line);

    /**
     * @brief 显式封存当前 active 块并提交版本。
     *        snapshot() 会自动调用，正常情况下调用方无需手动调用。
     */
    void publish();

    void clear();

    // 快照是一个发布边界：active 尾块被封存后以 shared_ptr 共享，
    // 不复制任何历史 Cell 数据。后续追加只影响新的 active 块，
    // 已发出的快照保持对应版本数据不变。
    ScrollbackSnapshot snapshot();
    const LogicalLine* lineAt(qsizetype index) const;
    qsizetype lineCount() const { return _lineCount; }
    qsizetype maxLines() const { return _maxLines; }
    qsizetype maxBytes() const { return _maxBytes; }
    qsizetype chunkLines() const { return _chunkLines; }
    quint64 version() const { return _version; }

    /**
     * @brief 调整行数与字节上限，立即触发淘汰以满足新约束。
     */
    void setLimits(qsizetype maxLines, qsizetype maxBytes);
    ScrollbackStatistics statistics() const;

private:
    // 已封存的分块及其在文档中的起始行号与有效字节数。
    struct StoredChunk
    {
        ScrollbackChunkPtr chunk;
        qsizetype firstLine{0};
        qsizetype effectiveBytes{0};
    };
    // 已被淘汰但可能仍被旧快照持有的分块。通过 weak_ptr 跟踪，
    // 当所有快照释放后才能真正回收内存。
    struct RetiredChunk
    {
        std::weak_ptr<const ScrollbackChunk> chunk;
        qsizetype bytes{0};
    };

    void ensureActive();
    void sealActive();
    void enforceLimits();
    void evictOldest();
    static qsizetype lineBytes(const LogicalLine& line);
    void collectRetired() const;

    std::deque<StoredChunk> _chunks;
    std::shared_ptr<ScrollbackChunk> _active;
    qsizetype _activeFirstLine{0};
    qsizetype _activeBytes{0};
    qsizetype _lineCount{0};
    qsizetype _cellCount{0};
    qsizetype _effectiveBytes{0};
    qsizetype _maxLines{DefaultMaxLines};
    qsizetype _maxBytes{DefaultMaxBytes};
    qsizetype _chunkLines{DefaultChunkLines};
    LineId _nextLineId{1};
    ChunkId _nextChunkId{1};
    quint64 _version{0};
    quint64 _evictedLines{0};
    quint64 _evictedChunks{0};
    mutable std::vector<RetiredChunk> _retired;
};

} // namespace NovaTerm
