/**
 * @file   GlyphRasterizer.h
 * @brief  字形栅格化与栅格化任务队列。
 *
 * GlyphRasterizer 调用 FreeType/QRawFont 把 GlyphKey 转换为 GlyphBitmap。
 * BoundedGlyphRasterQueue 是有界任务队列：可见字形优先于离屏字形
 * （deferred），按 generation 取消旧任务以避免过时栅格化浪费 CPU。
 */
#pragma once

#include "GlyphTypes.h"

#include <QFont>
#include <QHash>
#include <QQueue>

#include <optional>

namespace NovaTerm {

// 字形栅格化器。无状态，可跨线程调用。
class GlyphRasterizer
{
public:
    /**
     * @brief 栅格化一个字形。
     * @param key 字形标识。
     * @param font 字体。
     * @param cellWidth 单元格宽度（像素，用于约束字形边界）。
     * @param cellHeight 单元格高度（像素）。
     * @return 栅格化结果，含位图与几何度量。
     */
    GlyphBitmap rasterize(const GlyphKey& key, const QFont& font,
                          qreal cellWidth, qreal cellHeight) const;
};

// 有界栅格化任务队列。GUI 线程入队，worker 线程出队执行栅格化。
// 通过双队列（visible 优先于 deferred）与 pending 去重集，
// 保证同一字形不会重复栅格化，且视口内字形优先就绪。
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

    /**
     * @brief 入队一个栅格化任务。visible=true 的任务优先于 deferred。
     * @return true 表示入队成功；false 表示队列已满或已 stop。
     */
    bool enqueue(Task task);
    std::optional<Task> take();
    /**
     * @brief 取消所有早于指定 generation 的任务。用于字体变更后丢弃
     *        旧字形的栅格化请求。
     */
    void cancelBeforeGeneration(quint64 generation);
    void stop();
    bool stopped() const { return _stopped; }
    qsizetype size() const { return _visible.size() + _deferred.size(); }
    const Statistics& statistics() const { return _statistics; }

private:
    qsizetype _capacity;
    QQueue<Task> _visible;       // 可见字形任务，优先出队
    QQueue<Task> _deferred;     // 离屏字形任务，可见队列空时才出队
    QHash<GlyphKey, bool> _pending;  // 已入队任务的去重集合
    bool _stopped{false};
    Statistics _statistics;
};

} // namespace NovaTerm
