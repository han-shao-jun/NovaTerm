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
    _targetRefreshRate = hz;
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
    return lhs.startRow <= rhs.endRow && rhs.startRow <= lhs.endRow
        && lhs.startColumn <= rhs.endColumn
        && rhs.startColumn <= lhs.endColumn;
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
    if (!_timer.isActive())
        _timer.start(qMax(1, qRound(1000.0 / _targetRefreshRate)));
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
