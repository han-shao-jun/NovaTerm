#include "GlyphAtlas.h"

#include <QPainter>

#include <algorithm>
#include <limits>

namespace NovaTerm {

GlyphAtlas::GlyphAtlas(GlyphAtlasConfig config)
    : _config(std::move(config))
{
    _config.pageSize.setWidth(std::max(8, _config.pageSize.width()));
    _config.pageSize.setHeight(std::max(8, _config.pageSize.height()));
    _config.padding = std::max(0, _config.padding);
    _config.framesInFlight = std::max(1, _config.framesInFlight);
    _config.byteBudget = std::max(pageBytes(_config.pageSize),
                                  _config.byteBudget);
}

quint64 GlyphAtlas::pageBytes(const QSize& size)
{
    return quint64(size.width()) * quint64(size.height()) * 4;
}

std::optional<QRect> GlyphAtlas::allocate(Page& page, const QSize& size)
{
    const int paddedWidth = size.width() + 2 * _config.padding;
    const int paddedHeight = size.height() + 2 * _config.padding;
    if (paddedWidth > page.image.width() || paddedHeight > page.image.height())
        return std::nullopt;
    if (page.x + paddedWidth > page.image.width()) {
        page.x = 1;
        page.y += page.rowHeight;
        page.rowHeight = 0;
    }
    if (page.y + paddedHeight > page.image.height())
        return std::nullopt;
    QRect result(page.x + _config.padding, page.y + _config.padding,
                 size.width(), size.height());
    page.x += paddedWidth;
    page.rowHeight = std::max(page.rowHeight, paddedHeight);
    return result;
}

void GlyphAtlas::resetPage(Page& page, GlyphPixelFormat format,
                           quint64 frameNumber)
{
    page.format = format;
    ++page.generation;
    page.image.fill(Qt::transparent);
    page.image.setPixelColor(0, 0, Qt::white);
    page.x = 1;
    page.y = 1;
    page.rowHeight = 0;
    page.lastUsedFrame = frameNumber;
    page.retireFrame = frameNumber;
    page.dirtyRects.clear();
    // Recycling invalidates all old locations via page generation. No valid
    // command may sample untouched old rectangles, so only new insertions
    // need upload. Resource recovery is the explicit markAllDirty() path.
    page.fullDirty = false;
    ++_generation;
    ++_statistics.generationChanges;
}

int GlyphAtlas::createOrRecyclePage(GlyphPixelFormat format,
                                    quint64 frameNumber)
{
    const quint64 bytes = pageBytes(_config.pageSize);
    if (_statistics.currentBytes + bytes <= _config.byteBudget) {
        Page page;
        page.id = _pages.size();
        page.format = format;
        page.image = QImage(_config.pageSize, QImage::Format_RGBA8888);
        page.image.fill(Qt::transparent);
        page.image.setPixelColor(0, 0, Qt::white);
        page.lastUsedFrame = frameNumber;
        // A newly allocated texture-array layer has no resident entries yet.
        // Only subsequently inserted glyph rectangles are observable, so do
        // not misclassify page creation as resource recovery/full invalidation.
        page.fullDirty = false;
        _pages.push_back(std::move(page));
        _statistics.currentBytes += bytes;
        _statistics.peakBytes = std::max(_statistics.peakBytes,
                                         _statistics.currentBytes);
        ++_statistics.pageCreations;
        return _pages.size() - 1;
    }

    int candidate = -1;
    quint64 oldest = std::numeric_limits<quint64>::max();
    for (int index = 0; index < _pages.size(); ++index) {
        const Page& page = _pages[index];
        if (page.retireFrame > frameNumber || page.lastUsedFrame >= oldest)
            continue;
        candidate = index;
        oldest = page.lastUsedFrame;
    }
    if (candidate < 0)
        return -1;
    resetPage(_pages[candidate], format, frameNumber);
    ++_statistics.pageEvictions;
    return candidate;
}

std::optional<GlyphLocation> GlyphAtlas::insert(const GlyphBitmap& bitmap,
                                                quint64 frameNumber)
{
    if (bitmap.image.isNull()) {
        ++_statistics.allocationFailures;
        return std::nullopt;
    }
    const GlyphPixelFormat format = bitmap.key.format;
    int target = -1;
    std::optional<QRect> rect;
    for (int index = 0; index < _pages.size(); ++index) {
        if (_pages[index].format != format)
            continue;
        rect = allocate(_pages[index], bitmap.image.size());
        if (rect) {
            target = index;
            break;
        }
    }
    if (target < 0) {
        target = createOrRecyclePage(format, frameNumber);
        if (target >= 0)
            rect = allocate(_pages[target], bitmap.image.size());
    }
    if (target < 0 || !rect) {
        ++_statistics.allocationFailures;
        return std::nullopt;
    }

    Page& page = _pages[target];
    QPainter painter(&page.image);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(*rect, bitmap.image, bitmap.image.rect());
    painter.end();
    page.dirtyRects.push_back(*rect);
    page.lastUsedFrame = frameNumber;
    page.retireFrame = std::max(page.retireFrame,
                                frameNumber + quint64(_config.framesInFlight));
    ++_statistics.allocations;
    return GlyphLocation{page.id, page.generation, *rect,
                         bitmap.logicalRect, format};
}

bool GlyphAtlas::isValid(const GlyphLocation& location) const
{
    return location.pageId >= 0 && location.pageId < _pages.size()
        && _pages[location.pageId].generation == location.pageGeneration
        && _pages[location.pageId].format == location.format;
}

void GlyphAtlas::touch(const GlyphLocation& location, quint64 frameNumber)
{
    if (!isValid(location))
        return;
    _pages[location.pageId].lastUsedFrame = frameNumber;
}

void GlyphAtlas::pin(const GlyphLocation& location, quint64 retireFrame)
{
    if (!isValid(location))
        return;
    _pages[location.pageId].retireFrame = std::max(
        _pages[location.pageId].retireFrame, retireFrame);
}

QVector<QRect> GlyphAtlas::mergeDirtyRects(QVector<QRect> rects,
                                            const QRect& bounds)
{
    for (QRect& rect : rects)
        rect = rect.intersected(bounds);
    rects.erase(std::remove_if(rects.begin(), rects.end(),
                               [](const QRect& r) { return r.isEmpty(); }),
                rects.end());
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < rects.size() && !changed; ++i) {
            for (int j = i + 1; j < rects.size(); ++j) {
                const QRect expanded = rects[i].adjusted(-2, -2, 2, 2);
                if (!expanded.intersects(rects[j]))
                    continue;
                const QRect united = rects[i].united(rects[j]);
                const qint64 originalArea = qint64(rects[i].width()) * rects[i].height()
                    + qint64(rects[j].width()) * rects[j].height();
                const qint64 unitedArea = qint64(united.width()) * united.height();
                if (unitedArea > originalArea * 2)
                    continue;
                rects[i] = united;
                rects.removeAt(j);
                changed = true;
                break;
            }
        }
    }
    return rects;
}

QVector<GlyphAtlasUpload> GlyphAtlas::takeUploads(quint64 byteBudget)
{
    if (byteBudget == 0)
        byteBudget = _config.uploadBudgetPerFrame;
    QVector<GlyphAtlasUpload> result;
    quint64 used = 0;
    for (Page& page : _pages) {
        QVector<QRect> rects;
        if (page.fullDirty) {
            rects.push_back(page.image.rect());
        } else {
            _statistics.dirtyRectsBeforeMerge += quint64(page.dirtyRects.size());
            rects = mergeDirtyRects(page.dirtyRects, page.image.rect());
            _statistics.dirtyRectsAfterMerge += quint64(rects.size());
        }
        QVector<QRect> deferred;
        for (const QRect& rect : std::as_const(rects)) {
            const quint64 bytes = quint64(rect.width()) * rect.height() * 4;
            _statistics.requestedUploadBytes += bytes;
            if (used > 0 && used + bytes > byteBudget) {
                deferred.push_back(rect);
                _statistics.deferredUploadBytes += bytes;
                continue;
            }
            GlyphAtlasUpload upload;
            upload.pageId = page.id;
            upload.pageGeneration = page.generation;
            upload.rect = rect;
            upload.image = page.image.copy(rect);
            upload.fullPage = rect == page.image.rect();
            result.push_back(std::move(upload));
            used += bytes;
            _statistics.uploadedBytes += bytes;
            if (rect == page.image.rect())
                ++_statistics.fullPageUploads;
        }
        page.dirtyRects = std::move(deferred);
        page.fullDirty = false;
    }
    return result;
}

bool GlyphAtlas::hasPendingUploads() const
{
    for (const Page& page : _pages) {
        if (page.fullDirty || !page.dirtyRects.isEmpty())
            return true;
    }
    return false;
}

bool GlyphAtlas::hasFullPageUploads() const
{
    for (const Page& page : _pages) {
        if (page.fullDirty)
            return true;
    }
    return false;
}

void GlyphAtlas::markAllDirty()
{
    for (Page& page : _pages) {
        page.fullDirty = true;
        page.dirtyRects.clear();
    }
}

void GlyphAtlas::clear()
{
    _pages.clear();
    _statistics.currentBytes = 0;
    ++_generation;
    ++_statistics.generationChanges;
}

const QImage* GlyphAtlas::pageImage(int pageId) const
{
    return pageId >= 0 && pageId < _pages.size()
        ? &_pages[pageId].image : nullptr;
}

quint64 GlyphAtlas::pageGeneration(int pageId) const
{
    return pageId >= 0 && pageId < _pages.size()
        ? _pages[pageId].generation : 0;
}

} // namespace NovaTerm
