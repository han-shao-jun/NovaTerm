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
    static constexpr isize DefaultChunkLines = 1024;
    static constexpr isize DefaultMaxLines = 100'000;
    static constexpr isize MaximumMaxLines = 1'000'000;
    static constexpr isize DefaultMaxBytes = 256 * 1024 * 1024;

    explicit ChunkedScrollback(isize maxLines = DefaultMaxLines,
                               isize maxBytes = DefaultMaxBytes,
                               isize chunkLines = DefaultChunkLines);

    // 单线程拥有语义：隐式拷贝会共享可变 active 块并分叉行计数/ID 状态，
    // 移动同样无意义（含 deque 与 ID 计数器）。显式删除以落实「不可拷贝」
    // 的类注释；持有链（ScrollbackBuffer→Runtime→TerminalCore）随之自动
    // 不可拷贝。
    ChunkedScrollback(const ChunkedScrollback&) = delete;
    ChunkedScrollback& operator=(const ChunkedScrollback&) = delete;

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
    LineId append(const Cell* cells, isize columns,
                  bool hardBreak = true);

    /**
     * @brief 从最新逻辑行的尾部取走一段 Cell。
     * @param cellCount 期望取走的 Cell 数，不足时取走该行全部。
     * @param out 输出参数，接收取走的那一段（沿用原行的 id 与 hardBreak）。
     * @return true 表示取到内容；false 表示缓冲为空。
     * @note  服务于 libvterm 的 sb_popline —— 屏幕变高时它会反向取回紧邻
     *        屏幕顶部的那一行，也就是**最新**的历史行。一条逻辑行可能横跨
     *        多个屏幕行，因此只取走尾部一段；剩余部分留在历史中并把
     *        hardBreak 置为 false（尾段已回到活动屏幕，该逻辑行在历史里
     *        不再以硬换行结尾）。整行被取空时该行被移除。
     */
    bool takeNewestTail(isize cellCount, LogicalLine& out);

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

    /**
     * @brief 取尾部增量视图（不封存 active、不复制 ChunkView）。
     * @param sinceId  调用方上次已消费到的逻辑行 ID（其显示行需要重折）。
     * @param maxLines 尾部深拷贝行数上界；超过则置 resync 让调用方全量重排。
     * @param out      输出参数，填充 ScrollbackTail。
     * @note  高频路径专用：snapshot() 每次都会 publish()→sealActive() 从而在
     *        每批输出后封存小分块并复制全部 ChunkView；本方法只从末尾深拷贝
     *        少量逻辑行，不触碰分块组织，可每批调用而不产生碎片化。
     *        sinceId 被 sb_popline 取回（比现存最新还新）时，返回现存尾行以便
     *        调用方增量修正；sinceId 已被头部淘汰或落后过远时置 resync。
     */
    void tailFrom(LineId sinceId, isize maxLines, ScrollbackTail& out) const;

    const LogicalLine* lineAt(isize index) const;
    isize lineCount() const { return _lineCount; }
    isize maxLines() const { return _maxLines; }
    isize maxBytes() const { return _maxBytes; }
    isize chunkLines() const { return _chunkLines; }
    u64 version() const { return _version; }

    /**
     * @brief 调整行数与字节上限，立即触发淘汰以满足新约束。
     */
    void setLimits(isize maxLines, isize maxBytes);
    ScrollbackStatistics statistics() const;

private:
    // 已封存的分块及其在文档中的有效行区间与字节数。
    // 有效行是 lines[firstLine, firstLine + lineCount)：头部被 evictOldest
    // 淘汰的行与尾部被 makeNewestLineWritable 搬走的行都不在其中，但仍占用
    // 不可变分块的存储。
    struct StoredChunk
    {
        ScrollbackChunkPtr chunk;
        isize firstLine{0};
        isize lineCount{0};
        isize effectiveBytes{0};
        // 已搬到 active 块的尾行字节数，其字节已从 _effectiveBytes 扣除。
        // 整块退休时须扣除 byteSize - detachedBytes，避免二次扣减。
        isize detachedBytes{0};
    };
    // 已被淘汰但可能仍被旧快照持有的分块。通过 weak_ptr 跟踪，
    // 当所有快照释放后才能真正回收内存。
    struct RetiredChunk
    {
        std::weak_ptr<const ScrollbackChunk> chunk;
        isize bytes{0};
    };

    void ensureActive();
    void sealActive();
    void enforceLimits();
    void evictOldest();
    /**
     * @brief 保证最新逻辑行位于 active 块，从而可以原地改写。
     *
     * 已封存分块不可变（可能被多个快照共享），而 appendContinuation 与
     * takeNewestTail 都只改最新一行。复制整个分块（默认 1024 行）只为改一行
     * 代价过高，因此只把那一行复制进 active 块，并把它所属封存分块的有效行
     * 区间从尾部裁掉一行；分块被裁空即整块退休。
     * @return true 表示 active 块尾部现在就是最新逻辑行；false 表示缓冲为空。
     */
    bool makeNewestLineWritable();
    void retireChunk(StoredChunk& stored, bool countAsEvicted);
    static isize lineBytes(const LogicalLine& line);
    void collectRetired() const;

    std::deque<StoredChunk> _chunks;
    std::shared_ptr<ScrollbackChunk> _active;
    isize _activeFirstLine{0};
    isize _activeBytes{0};
    isize _lineCount{0};
    isize _cellCount{0};
    isize _effectiveBytes{0};
    isize _maxLines{DefaultMaxLines};
    isize _maxBytes{DefaultMaxBytes};
    isize _chunkLines{DefaultChunkLines};
    LineId _nextLineId{1};
    ChunkId _nextChunkId{1};
    u64 _version{0};
    u64 _evictedLines{0};
    u64 _evictedChunks{0};
    mutable std::vector<RetiredChunk> _retired;
};

} // namespace NovaTerm
