#include "GlyphRasterizer.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>

namespace NovaTerm {

GlyphBitmap GlyphRasterizer::rasterize(const GlyphKey& key, const QFont& font,
                                       qreal cellWidth, qreal cellHeight) const
{
    GlyphBitmap result;
    result.key = key;
    result.sourceGeneration = key.fontGeneration;
    result.cellSpan = std::max(1, key.cellSpan);
    if (key.cluster.isEmpty()) {
        result.diagnostic = QStringLiteral("empty cluster");
        return result;
    }
    const qreal scale = std::max(1, key.scale1024) / 1024.0;
    QFont rasterFont(font);
    rasterFont.setWeight(QFont::Weight(key.weight));
    rasterFont.setItalic(key.italic);
    const QFontMetricsF metrics(rasterFont);
    const qreal advance = metrics.horizontalAdvance(key.cluster);
    const QSizeF logicalExtent(
        std::max(advance, cellWidth * result.cellSpan),
        std::max(cellHeight, metrics.height()));
    result.advance = advance;
    result.bearingX = metrics.leftBearing(key.cluster.front());
    result.bearingY = metrics.ascent();
    result.baseline = metrics.ascent();
    const QSize pixels(std::max(1, qCeil(logicalExtent.width() * scale)),
                       std::max(1, qCeil(logicalExtent.height() * scale)));
    // The bitmap already contains the baseline offset because drawText()
    // paints at metrics.ascent(). Its GPU quad must therefore start at the
    // cell-local origin. Applying -ascent again moves every glyph one line
    // upward while cursor/selection overlays remain on the cell grid.
    // Derive the extent from physical pixels to keep fractional DPR sampling
    // aligned with the atlas upload exactly as the pre-P5 path did.
    result.logicalRect = QRectF(0, 0,
                                pixels.width() / scale,
                                pixels.height() / scale);
    QImage image(pixels, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(scale);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(rasterFont);
    painter.setPen(Qt::white);
    painter.drawText(QPointF(0, metrics.ascent()), key.cluster);
    painter.end();
    result.image = std::move(image);
    return result;
}

BoundedGlyphRasterQueue::BoundedGlyphRasterQueue(qsizetype capacity)
    : _capacity(std::max<qsizetype>(1, capacity))
{
}

bool BoundedGlyphRasterQueue::enqueue(Task task)
{
    if (_stopped)
        return false;
    if (_pending.contains(task.key)) {
        ++_statistics.deduplicated;
        return true;
    }
    if (size() >= _capacity) {
        ++_statistics.rejected;
        return false;
    }
    _pending.insert(task.key, task.visible);
    (task.visible ? _visible : _deferred).enqueue(std::move(task));
    ++_statistics.enqueued;
    _statistics.peakDepth = std::max(_statistics.peakDepth, size());
    return true;
}

std::optional<BoundedGlyphRasterQueue::Task> BoundedGlyphRasterQueue::take()
{
    if (_visible.isEmpty() && _deferred.isEmpty())
        return std::nullopt;
    Task task = !_visible.isEmpty() ? _visible.dequeue() : _deferred.dequeue();
    _pending.remove(task.key);
    return task;
}

void BoundedGlyphRasterQueue::cancelBeforeGeneration(quint64 generation)
{
    auto filter = [this, generation](QQueue<Task>& queue) {
        QQueue<Task> kept;
        while (!queue.isEmpty()) {
            Task task = queue.dequeue();
            if (task.key.fontGeneration < generation) {
                _pending.remove(task.key);
                ++_statistics.staleDropped;
            } else {
                kept.enqueue(std::move(task));
            }
        }
        queue.swap(kept);
    };
    filter(_visible);
    filter(_deferred);
}

void BoundedGlyphRasterQueue::stop()
{
    _statistics.cancelled += quint64(size());
    _visible.clear();
    _deferred.clear();
    _pending.clear();
    _stopped = true;
}

} // namespace NovaTerm
