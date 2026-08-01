#pragma once

#include "GlyphTypes.h"

#include <QHash>
#include <QImage>
#include <QRect>
#include <QVector>

#include <optional>

namespace NovaTerm {

struct GlyphAtlasConfig
{
    QSize pageSize{2048, 2048};
    quint64 byteBudget{64ull * 1024ull * 1024ull};
    quint64 uploadBudgetPerFrame{4ull * 1024ull * 1024ull};
    int padding{1};
    int framesInFlight{3};
};

struct GlyphAtlasStatistics
{
    quint64 allocations{0};
    quint64 allocationFailures{0};
    quint64 pageCreations{0};
    quint64 pageEvictions{0};
    quint64 generationChanges{0};
    quint64 requestedUploadBytes{0};
    quint64 uploadedBytes{0};
    quint64 deferredUploadBytes{0};
    quint64 fullPageUploads{0};
    quint64 dirtyRectsBeforeMerge{0};
    quint64 dirtyRectsAfterMerge{0};
    quint64 currentBytes{0};
    quint64 peakBytes{0};
};

struct GlyphAtlasUpload
{
    int pageId{-1};
    quint64 pageGeneration{0};
    QRect rect;
    QImage image;
    bool fullPage{false};
    quint64 bytes() const { return quint64(rect.width()) * rect.height() * 4; }
};

class GlyphAtlas
{
public:
    explicit GlyphAtlas(GlyphAtlasConfig config = {});

    std::optional<GlyphLocation> insert(const GlyphBitmap& bitmap,
                                        quint64 frameNumber);
    bool isValid(const GlyphLocation& location) const;
    void touch(const GlyphLocation& location, quint64 frameNumber);
    void pin(const GlyphLocation& location, quint64 retireFrame);
    QVector<GlyphAtlasUpload> takeUploads(quint64 byteBudget = 0);
    bool hasPendingUploads() const;
    bool hasFullPageUploads() const;
    void markAllDirty();
    void clear();

    int pageCount() const { return _pages.size(); }
    quint64 generation() const { return _generation; }
    const GlyphAtlasConfig& config() const { return _config; }
    const GlyphAtlasStatistics& statistics() const { return _statistics; }
    const QImage* pageImage(int pageId) const;
    quint64 pageGeneration(int pageId) const;

private:
    struct Page {
        int id{-1};
        GlyphPixelFormat format{GlyphPixelFormat::Alpha8};
        quint64 generation{1};
        QImage image;
        int x{1};
        int y{1};
        int rowHeight{0};
        quint64 lastUsedFrame{0};
        quint64 retireFrame{0};
        QVector<QRect> dirtyRects;
        bool fullDirty{true};
    };

    static quint64 pageBytes(const QSize& size);
    static QVector<QRect> mergeDirtyRects(QVector<QRect> rects,
                                           const QRect& bounds);
    int createOrRecyclePage(GlyphPixelFormat format, quint64 frameNumber);
    std::optional<QRect> allocate(Page& page, const QSize& size);
    void resetPage(Page& page, GlyphPixelFormat format, quint64 frameNumber);

    GlyphAtlasConfig _config;
    QVector<Page> _pages;
    quint64 _generation{1};
    GlyphAtlasStatistics _statistics;
};

} // namespace NovaTerm
