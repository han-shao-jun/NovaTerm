#pragma once

#include "GlyphAtlas.h"

#include <QHash>

#include <optional>

namespace NovaTerm {

struct GlyphCacheStatistics
{
    quint64 hits{0};
    quint64 misses{0};
    quint64 inserts{0};
    quint64 staleEntries{0};
    quint64 failed{0};
};

class GlyphCache
{
public:
    explicit GlyphCache(GlyphAtlasConfig config = {});

    std::optional<GlyphLocation> find(const GlyphKey& key,
                                      quint64 frameNumber);
    std::optional<GlyphLocation> insert(const GlyphBitmap& bitmap,
                                        quint64 frameNumber);
    void invalidateFontGeneration(quint64 generation);
    void clear();

    GlyphAtlas& atlas() { return _atlas; }
    const GlyphAtlas& atlas() const { return _atlas; }
    const GlyphCacheStatistics& statistics() const { return _statistics; }

private:
    struct Entry { GlyphLocation location; quint64 lastUsedFrame{0}; };
    GlyphAtlas _atlas;
    QHash<GlyphKey, Entry> _entries;
    GlyphCacheStatistics _statistics;
};

} // namespace NovaTerm
