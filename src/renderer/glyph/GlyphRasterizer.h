#pragma once

#include "GlyphTypes.h"

#include <QFont>
#include <QHash>
#include <QQueue>

#include <optional>

namespace NovaTerm {

class GlyphRasterizer
{
public:
    GlyphBitmap rasterize(const GlyphKey& key, const QFont& font,
                          qreal cellWidth, qreal cellHeight) const;
};

class BoundedGlyphRasterQueue
{
public:
    struct Statistics {
        quint64 enqueued{0};
        quint64 deduplicated{0};
        quint64 rejected{0};
        quint64 cancelled{0};
        quint64 staleDropped{0};
        qsizetype peakDepth{0};
    };
    struct Task { GlyphKey key; QFont font; qreal cellWidth; qreal cellHeight; bool visible; };

    explicit BoundedGlyphRasterQueue(qsizetype capacity = 512);
    bool enqueue(Task task);
    std::optional<Task> take();
    void cancelBeforeGeneration(quint64 generation);
    void stop();
    bool stopped() const { return _stopped; }
    qsizetype size() const { return _visible.size() + _deferred.size(); }
    const Statistics& statistics() const { return _statistics; }

private:
    qsizetype _capacity;
    QQueue<Task> _visible;
    QQueue<Task> _deferred;
    QHash<GlyphKey, bool> _pending;
    bool _stopped{false};
    Statistics _statistics;
};

} // namespace NovaTerm
