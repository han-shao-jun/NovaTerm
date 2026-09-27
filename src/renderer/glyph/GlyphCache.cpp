/**
 * @file   GlyphCache.cpp
 * @brief  字形缓存实现。
 *
 * 详见 GlyphCache.h。本文件实现 GlyphKey → GlyphLocation 的查表、
 * 插入与按 generation 失效。命中路径更新 LRU 帧号；未命中路径
 * 由调用方栅格化后回填。
 */
#include "GlyphCache.h"

namespace NovaTerm {

GlyphCache::GlyphCache(GlyphAtlasConfig config)
    : _atlas(std::move(config))
{
}

std::optional<GlyphLocation> GlyphCache::findEntry(const GlyphKey& key,
                                                   quint64 frameNumber)
{
    auto found = _entries.find(key);
    if (found == _entries.end())
        return std::nullopt;
    // 条目存在但其所在 atlas 页可能已被回收（generation 变更）。
    // 此时位置无效，清理条目并按未命中处理。
    if (!_atlas.isValid(found->location)) {
        _entries.erase(found);
        ++_statistics.staleEntries;
        return std::nullopt;
    }
    found->lastUsedFrame = frameNumber;
    _atlas.touch(found->location, frameNumber);
    return found->location;
}

std::optional<GlyphLocation> GlyphCache::find(const GlyphKey& key,
                                              quint64 frameNumber)
{
    const auto location = findEntry(key, frameNumber);
    if (location)
        ++_statistics.hits;
    else
        ++_statistics.misses;
    return location;
}

bool GlyphCache::purgeStaleEntries()
{
    for (auto it = _entries.begin(); it != _entries.end();) {
        if (_atlas.isValid(it->location)) {
            ++it;
            continue;
        }
        it = _entries.erase(it);
        ++_statistics.staleEntries;
    }
    return _entries.size() >= MaxEntries;
}

std::optional<GlyphLocation> GlyphCache::insert(const GlyphBitmap& bitmap,
                                                quint64 frameNumber)
{
    // 栅格化期间字体可能已变更：sourceGeneration 与 key.fontGeneration
    // 不一致说明这个位图已过时，直接丢弃避免写入错误字形。
    if (bitmap.sourceGeneration != bitmap.key.fontGeneration) {
        ++_statistics.failed;
        return std::nullopt;
    }
    // 去重查找走不记账的 findEntry：insert 路径的命中不应计入
    // glyphCacheHits/Misses 诊断指标（旧实现把每次插入都记成一次 miss）。
    if (auto existing = findEntry(bitmap.key, frameNumber))
        return existing;
    auto location = _atlas.insert(bitmap, frameNumber);
    if (!location) {
        ++_statistics.failed;
        return std::nullopt;
    }
    if (_entries.size() >= MaxEntries) {
        // 先清扫死条目；病态流量下仍超上限才整表清空。清扫只在跨过
        // 上限的插入时发生，均摊成本可忽略。
        if (purgeStaleEntries())
            _entries.clear();
    }
    _entries.insert(bitmap.key, Entry{*location, frameNumber});
    ++_statistics.inserts;
    return location;
}

void GlyphCache::invalidateFontGeneration(quint64 generation)
{
    for (auto it = _entries.begin(); it != _entries.end();) {
        if (it.key().fontGeneration != generation)
            it = _entries.erase(it);
        else
            ++it;
    }
}

void GlyphCache::clear()
{
    _entries.clear();
    _atlas.clear();
}

} // namespace NovaTerm
