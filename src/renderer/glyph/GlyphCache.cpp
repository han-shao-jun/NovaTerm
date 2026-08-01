#include "GlyphCache.h"

namespace NovaTerm {

GlyphCache::GlyphCache(GlyphAtlasConfig config)
    : _atlas(std::move(config))
{
}

std::optional<GlyphLocation> GlyphCache::find(const GlyphKey& key,
                                              quint64 frameNumber)
{
    auto found = _entries.find(key);
    if (found == _entries.end()) {
        ++_statistics.misses;
        return std::nullopt;
    }
    if (!_atlas.isValid(found->location)) {
        _entries.erase(found);
        ++_statistics.staleEntries;
        ++_statistics.misses;
        return std::nullopt;
    }
    found->lastUsedFrame = frameNumber;
    _atlas.touch(found->location, frameNumber);
    ++_statistics.hits;
    return found->location;
}

std::optional<GlyphLocation> GlyphCache::insert(const GlyphBitmap& bitmap,
                                                quint64 frameNumber)
{
    if (bitmap.sourceGeneration != bitmap.key.fontGeneration) {
        ++_statistics.failed;
        return std::nullopt;
    }
    if (auto existing = find(bitmap.key, frameNumber))
        return existing;
    auto location = _atlas.insert(bitmap, frameNumber);
    if (!location) {
        ++_statistics.failed;
        return std::nullopt;
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
