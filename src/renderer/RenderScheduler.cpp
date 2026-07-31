#include "RenderScheduler.h"

#include <algorithm>
#include <utility>

namespace NovaTerm {

namespace {
constexpr int MaxDirtyRegions = 32;
constexpr double FullFrameCoverage = 0.60;
}

RenderScheduler::RenderScheduler(QObject* parent)
    : QObject(parent)
{
    _timer.setSingleShot(true);
    _timer.setTimerType(Qt::PreciseTimer);
    connect(&_timer, &QTimer::timeout, this, &RenderScheduler::submitFrame);
}

void RenderScheduler::setViewport(int columns, int rows)
{
    columns = qMax(0, columns);
    rows = qMax(0, rows);
    if (_columns == columns && _rows == rows)
        return;
    _columns = columns;
    _rows = rows;
    scheduleFullFrame();
}

void RenderScheduler::setTargetRefreshRate(int hz)
{
    if (hz != 60 && hz != 120 && hz != 144)
        hz = 60;
    if (_targetRefreshRate == hz)
        return;
    _targetRefreshRate = hz;
    _frameIntervalRemainderMs = 0.0;
}

void RenderScheduler::schedule(const DirtyRegion& region)
{
    if (_columns <= 0 || _rows <= 0)
        return;

    DirtyRegion clipped{
        std::clamp(region.startRow, 0, _rows),
        std::clamp(region.endRow, 0, _rows),
        std::clamp(region.startColumn, 0, _columns),
        std::clamp(region.endColumn, 0, _columns)
    };
    if (clipped.isEmpty())
        return;

    ++_statistics.dirtyRegionsReceived;
    if (_timer.isActive())
        ++_statistics.coalescedFrameRequests;
    _pending.push_back(clipped);
    mergePending();
    if (shouldPromoteToFullFrame()) {
        _pending.clear();
        _fullFramePending = true;
    }
    armTimer();
}

void RenderScheduler::scheduleFullFrame()
{
    if (_timer.isActive())
        ++_statistics.coalescedFrameRequests;
    _pending.clear();
    _fullFramePending = true;
    _overlayPending = true;
    armTimer();
}

void RenderScheduler::scheduleOverlay()
{
    if (_timer.isActive())
        ++_statistics.coalescedFrameRequests;
    _overlayPending = true;
    armTimer();
}

void RenderScheduler::cancel()
{
    _timer.stop();
    _pending.clear();
    _fullFramePending = false;
    _overlayPending = false;
}

bool RenderScheduler::touches(const DirtyRegion& lhs, const DirtyRegion& rhs)
{
    const bool rowsOverlap = lhs.startRow < rhs.endRow
        && rhs.startRow < lhs.endRow;
    const bool columnsOverlap = lhs.startColumn < rhs.endColumn
        && rhs.startColumn < lhs.endColumn;
    const bool rowsAdjacent = lhs.endRow == rhs.startRow
        || rhs.endRow == lhs.startRow;
    const bool columnsAdjacent = lhs.endColumn == rhs.startColumn
        || rhs.endColumn == lhs.startColumn;
    return (rowsOverlap && columnsOverlap)
        || (rowsAdjacent && columnsOverlap)
        || (columnsAdjacent && rowsOverlap);
}

DirtyRegion RenderScheduler::united(const DirtyRegion& lhs,
                                    const DirtyRegion& rhs)
{
    return {
        qMin(lhs.startRow, rhs.startRow),
        qMax(lhs.endRow, rhs.endRow),
        qMin(lhs.startColumn, rhs.startColumn),
        qMax(lhs.endColumn, rhs.endColumn)
    };
}

void RenderScheduler::armTimer()
{
    if (_timer.isActive())
        return;
    // QTimer accepts integer milliseconds. Carry the rounding error between
    // frames so 60/120/144 Hz converge to the requested average instead of
    // being permanently rounded to 17/8/7 ms.
    const double exactInterval = 1000.0 / _targetRefreshRate
        + _frameIntervalRemainderMs;
    const int interval = qMax(1, qRound(exactInterval));
    _frameIntervalRemainderMs = exactInterval - interval;
    _timer.start(interval);
}

void RenderScheduler::mergePending()
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < _pending.size() && !changed; ++i) {
            for (int j = i + 1; j < _pending.size(); ++j) {
                if (!touches(_pending[i], _pending[j]))
                    continue;
                _pending[i] = united(_pending[i], _pending[j]);
                _pending.removeAt(j);
                changed = true;
                break;
            }
        }
    }
}

bool RenderScheduler::shouldPromoteToFullFrame() const
{
    if (_pending.size() > MaxDirtyRegions)
        return true;
    const qint64 screenArea = qint64(_columns) * _rows;
    if (screenArea <= 0)
        return false;
    qint64 dirtyArea = 0;
    for (const DirtyRegion& region : _pending) {
        dirtyArea += qint64(region.endRow - region.startRow)
            * (region.endColumn - region.startColumn);
    }
    return double(dirtyArea) / double(screenArea) >= FullFrameCoverage;
}

void RenderScheduler::submitFrame()
{
    if (!_fullFramePending && !_overlayPending && _pending.isEmpty())
        return;

    ++_statistics.framesRequested;
    if (_fullFramePending)
        ++_statistics.fullFrames;
    _statistics.dirtyRegionsSubmitted += quint64(_pending.size());

    const QVector<DirtyRegion> regions = std::move(_pending);
    _pending.clear();
    const bool fullFrame = std::exchange(_fullFramePending, false);
    const bool overlayDirty = std::exchange(_overlayPending, false);
    emit frameRequested(regions, fullFrame, overlayDirty);
}

} // namespace NovaTerm
