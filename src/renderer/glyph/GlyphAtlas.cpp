/**
 * @file   GlyphAtlas.cpp
 * @brief  字形纹理 atlas 实现。
 *
 * 详见 GlyphAtlas.h。本文件实现 shelf-pack 矩形分配、页创建/回收、
 * 脏矩形合并与按字节预算取出上传请求。
 */
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
    // byteBudget 至少要能放下一页，否则永远无法分配。
    _config.byteBudget = std::max(pageBytes(_config.pageSize),
                                  _config.byteBudget);
}

quint64 GlyphAtlas::pageBytes(const QSize& size)
{
    return quint64(size.width()) * quint64(size.height()) * 4;
}

// shelf-pack 矩形分配：在页内从左到右、从上到下放置字形。
// 当前行放不下时换行；整页放不下时返回 std::nullopt。
std::optional<QRect> GlyphAtlas::allocate(Page& page, const QSize& size)
{
    const int paddedWidth = size.width() + 2 * _config.padding;
    const int paddedHeight = size.height() + 2 * _config.padding;
    if (paddedWidth > page.image.width() || paddedHeight > page.image.height())
        return std::nullopt;
    // 当前行剩余宽度不够：换行，y 前进到下一行顶部。
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
    // (0,0) 像素设为白色作为"页存活"哨兵，用于诊断纹理是否被错误清空。
    page.image.setPixelColor(0, 0, Qt::white);
    page.x = 1;
    page.y = 1;
    page.rowHeight = 0;
    page.lastUsedFrame = frameNumber;
    page.retireFrame = frameNumber;
    page.dirtyRects.clear();
    // 回收通过递增 page.generation 使所有旧 location 失效。没有任何有效
    // 命令会采样到旧的未触碰矩形，因此只需上传后续新插入的字形矩形。
    // 整页脏标记仅用于显式的资源恢复路径（markAllDirty）。
    page.fullDirty = false;
    ++_generation;
    ++_statistics.generationChanges;
}

int GlyphAtlas::createOrRecyclePage(GlyphPixelFormat format,
                                    quint64 frameNumber)
{
    const quint64 bytes = pageBytes(_config.pageSize);
    // 内存预算允许：直接新建一页。
    if (_statistics.currentBytes + bytes <= _config.byteBudget) {
        Page page;
        page.id = _pages.size();
        page.format = format;
        page.image = QImage(_config.pageSize, QImage::Format_RGBA8888);
        page.image.fill(Qt::transparent);
        page.image.setPixelColor(0, 0, Qt::white);
        page.lastUsedFrame = frameNumber;
        // 新分配的纹理数组层还没有任何已驻留条目。只有后续插入的字形矩形
        // 是可观测的，因此不要把页创建误判为资源恢复/整页失效。
        page.fullDirty = false;
        _pages.push_back(std::move(page));
        _statistics.currentBytes += bytes;
        _statistics.peakBytes = std::max(_statistics.peakBytes,
                                         _statistics.currentBytes);
        ++_statistics.pageCreations;
        return _pages.size() - 1;
    }

    // 预算已满：在已退役（retireFrame <= frameNumber）的页中选最久未用的回收。
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
    // 优先在现有同格式页中尝试分配。
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
    // 现有页都放不下：新建或回收一页。
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
    // retireFrame 推后 framesInFlight 帧，保证在途帧渲染期间该页不被回收。
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

// 合并相邻/重叠的脏矩形以减少上传次数。两矩形在扩展 2px 后相交即视为
// 可合并；但若合并后面积超过原面积 2 倍则保留独立上传，避免过度放大。
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
            // 首块必发以保证帧推进；后续块超预算则延后到下一帧。
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
