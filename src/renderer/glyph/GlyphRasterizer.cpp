/**
 * @file   GlyphRasterizer.cpp
 * @brief  字形栅格化与栅格化任务队列实现。
 *
 * 详见 GlyphRasterizer.h。rasterize() 用 QPainter 把字符簇绘制到
 * 透明背景的 QImage 上，并记录 bearing/advance/baseline 等几何度量。
 * BoundedGlyphRasterQueue 实现双队列优先级调度与按 generation 取消。
 */
#include "GlyphRasterizer.h"

#include "core/ThreadNaming.h"

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
    // 逻辑尺寸至少覆盖一个 cell（宽字符占 2 cell），避免位图被裁剪。
    const QSizeF logicalExtent(
        std::max(advance, cellWidth * result.cellSpan),
        std::max(cellHeight, metrics.height()));
    result.advance = advance;
    result.bearingX = metrics.leftBearing(key.cluster.front());
    result.bearingY = metrics.ascent();
    result.baseline = metrics.ascent();
    const QSize pixels(std::max(1, qCeil(logicalExtent.width() * scale)),
                       std::max(1, qCeil(logicalExtent.height() * scale)));
    // 每个位图上限 256 KiB；异步结果队列 32 个，避免异常字体/DPR 放大内存。
    if (qint64(pixels.width()) * pixels.height() > MaxBitmapBytes / 4) {
        result.diagnostic = QStringLiteral("glyph bitmap exceeds 256 KiB");
        return result;
    }
    // 位图本身已含 baseline 偏移（drawText 在 metrics.ascent() 处绘制），
    // 因此 GPU quad 必须从 cell 本地原点起算。若再叠加 -ascent 会让每个
    // 字形整体上移一行，而光标/选区叠加层仍在 cell 网格上，两者错位。
    // 物理像素派生 logicalRect，保证分数 DPR 采样与 atlas 上传严格对齐。
    result.logicalRect = QRectF(0, 0,
                                pixels.width() / scale,
                                pixels.height() / scale);
    QImage image(pixels, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(scale);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(rasterFont);
    // 用白色绘制：灰度字形在着色阶段再以前景色 tinting，彩色字形（emoji）
    // 由调用方单独处理。
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
    // 同一 GlyphKey 已在队列中：去重，不重复栅格化。
    if (_pending.contains(task.key)) {
        ++_statistics.deduplicated;
        return true;
    }
    if (size() >= _capacity) {
        ++_statistics.rejected;
        return false;
    }
    _pending.insert(task.key, task.visible);
    // visible 任务入 _visible 队列优先出队；deferred 任务入 _deferred。
    (task.visible ? _visible : _deferred).enqueue(std::move(task));
    ++_statistics.enqueued;
    _statistics.peakDepth = std::max(_statistics.peakDepth, size());
    return true;
}

std::optional<BoundedGlyphRasterQueue::Task> BoundedGlyphRasterQueue::take()
{
    if (_visible.isEmpty() && _deferred.isEmpty())
        return std::nullopt;
    // 优先取 visible；为空时才取 deferred，保证视口内字形优先就绪。
    Task task = !_visible.isEmpty() ? _visible.dequeue() : _deferred.dequeue();
    _pending.remove(task.key);
    return task;
}

void BoundedGlyphRasterQueue::cancelBeforeGeneration(quint64 generation)
{
    // 重建队列，丢弃 fontGeneration < generation 的过时任务。
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

void AsyncGlyphRasterizer::setReadyCallback(std::function<void()> callback)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    _ready = std::move(callback);
}

bool AsyncGlyphRasterizer::enqueue(Task task)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    if (_stopped || task.key.fontGeneration < _generation
        || task.key.cluster.size() > MaxClusterUnits)
        return false;
    if (_pending.contains(task.key))
        return true;
    if (_pending.size() >= MaxPendingTasks)
        return false;
    // 线程创建可能抛异常；在修改任务队列之前创建，保留原状态。
    if (!_worker.joinable())
        _worker = std::thread([this] { run(); });
    const auto key = task.key;
    if (!_queue.enqueue(std::move(task)))
        return false;
    _pending.insert(key, true);
    _changed.notify_one();
    return true;
}

void AsyncGlyphRasterizer::run()
{
    setCurrentThreadName("nvterm-glyph");
    std::unique_lock<std::mutex> lock(_mutex);
    while (!_stopped) {
        _changed.wait(lock, [this] {
            return _stopped || (_queue.size() > 0 && _results.size() < MaxReadyBitmaps);
        });
        if (_stopped)
            break;
        auto task = _queue.take();
        lock.unlock();
        auto bitmap = GlyphRasterizer().rasterize(task->key, task->font,
                                                  task->cellWidth, task->cellHeight);
        lock.lock();
        if (_stopped)
            break;
        if (bitmap.sourceGeneration < _generation) {
            _pending.remove(bitmap.key);
            continue;
        }
        _results.push_back(std::move(bitmap));
        auto notify = !_notified ? _ready : std::function<void()>{};
        _notified = true;
        lock.unlock();
        if (notify)
            notify();
        lock.lock();
    }
}

std::deque<GlyphBitmap> AsyncGlyphRasterizer::takeResults()
{
    const std::lock_guard<std::mutex> lock(_mutex);
    std::deque<GlyphBitmap> result;
    result.swap(_results);
    for (const auto& bitmap : result) {
        // 失败结果保留去重标记，直到 generation 变化；防止每帧重试耗尽 CPU。
        if (!bitmap.image.isNull())
            _pending.remove(bitmap.key);
        else
            _pending[bitmap.key] = false;
    }
    _notified = false;
    _changed.notify_one();
    return result;
}

void AsyncGlyphRasterizer::cancelBeforeGeneration(quint64 generation)
{
    const std::lock_guard<std::mutex> lock(_mutex);
    _generation = generation;
    _queue.cancelBeforeGeneration(generation);
    for (auto it = _pending.begin(); it != _pending.end();) {
        if (it.key().fontGeneration < generation)
            it = _pending.erase(it);
        else
            ++it;
    }
    _results.erase(std::remove_if(_results.begin(), _results.end(),
        [generation](const GlyphBitmap& bitmap) {
            return bitmap.sourceGeneration < generation;
        }), _results.end());
    _notified = false;
    _changed.notify_one();
}

void AsyncGlyphRasterizer::stop()
{
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _stopped = true;
        _queue.stop();
        _pending.clear();
        _results.clear();
    }
    _changed.notify_one();
    if (_worker.joinable())
        _worker.join();
}

qsizetype AsyncGlyphRasterizer::size() const
{
    const std::lock_guard<std::mutex> lock(_mutex);
    return std::count(_pending.cbegin(), _pending.cend(), true);
}

AsyncGlyphRasterizer::Statistics AsyncGlyphRasterizer::statistics() const
{
    const std::lock_guard<std::mutex> lock(_mutex);
    return _queue.statistics();
}

} // namespace NovaTerm
