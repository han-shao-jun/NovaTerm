/**
 * @file   GlyphCache.h
 * @brief  字形缓存：GlyphKey → GlyphLocation 的查表层。
 *
 * GlyphCache 在 GlyphAtlas 之上提供按 GlyphKey 查找字形位置的能力。
 * 命中即返回 atlas 中的位置；未命中时由调用方栅格化后调用 insert()
 * 写入 atlas。缓存条目按 lastUsedFrame 实现 LRU 语义，供 atlas 页
 * 回收参考。fontGeneration 不匹配的旧条目会在 invalidateFontGeneration()
 * 中被整批清除。
 */
#pragma once

#include "GlyphAtlas.h"

#include <QHash>

#include <optional>

namespace NovaTerm {

// 字形缓存统计信息，用于诊断命中率与淘汰情况。
struct GlyphCacheStatistics
{
    quint64 hits{0};           // 命中次数
    quint64 misses{0};         // 未命中次数（含因 atlas 页失效导致的 miss）
    quint64 inserts{0};        // 成功插入次数
    quint64 staleEntries{0};  // 因 atlas 页失效被清理的条目数
    quint64 failed{0};         // 插入失败次数（generation 不匹配或 atlas 满）
};

// 字形缓存。线程模型与 GlyphAtlas 一致：单线程（GUI 线程）独占使用。
class GlyphCache
{
public:
    explicit GlyphCache(GlyphAtlasConfig config = {});

    /**
     * @brief 查找字形位置。命中时会更新该条目的 lastUsedFrame 与
     *        atlas 页的 lastUsedFrame，影响后续页回收决策。
     * @param key 字形标识。
     * @param frameNumber 当前帧号，用于 LRU。
     * @return 命中返回 GlyphLocation；未命中或条目已失效返回 std::nullopt。
     */
    std::optional<GlyphLocation> find(const GlyphKey& key,
                                      quint64 frameNumber);

    /**
     * @brief 插入栅格化结果到 atlas 并建立 GlyphKey → 位置映射。
     *        若 bitmap 的 sourceGeneration 与 key.fontGeneration 不一致，
     *        说明栅格化时字体已变更，结果作废直接返回 std::nullopt。
     * @param bitmap 栅格化结果。
     * @param frameNumber 当前帧号。
     * @return 插入成功返回 GlyphLocation；atlas 无可用页或 generation
     *         不匹配时返回 std::nullopt。
     */
    std::optional<GlyphLocation> insert(const GlyphBitmap& bitmap,
                                        quint64 frameNumber);

    /**
     * @brief 清除所有 fontGeneration != generation 的条目。字体变更后
     *        用于丢弃所有旧字形的缓存项。
     */
    void invalidateFontGeneration(quint64 generation);

    void clear();

    GlyphAtlas& atlas() { return _atlas; }
    const GlyphAtlas& atlas() const { return _atlas; }
    const GlyphCacheStatistics& statistics() const { return _statistics; }

private:
    // 缓存条目：atlas 中的位置 + 最近使用帧号（LRU 依据）。
    struct Entry { GlyphLocation location; quint64 lastUsedFrame{0}; };
    GlyphAtlas _atlas;
    QHash<GlyphKey, Entry> _entries;
    GlyphCacheStatistics _statistics;
};

} // namespace NovaTerm
