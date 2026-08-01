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
    _clock.start();
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
    _nextDeadlineNanoseconds = 0;
}

void RenderScheduler::schedule(const DirtyRegion& region, quint64 revision)
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
    _pendingContentRevision = std::max(_pendingContentRevision, revision);
    if (_timer.isActive())
        ++_statistics.coalescedFrameRequests;
    if (_fullFramePending) {
        armTimer();
        return;
    }
    _pending.push_back(clipped);
    mergePending();
    if (shouldPromoteToFullFrame()) {
        _pending.clear();
        _fullFramePending = true;
    }
    armTimer();
}

void RenderScheduler::scheduleFullFrame(quint64 revision)
{
    if (_timer.isActive())
        ++_statistics.coalescedFrameRequests;
    _pending.clear();
    _fullFramePending = true;
    _overlayPending = true;
    _pendingContentRevision = std::max(_pendingContentRevision, revision);
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
    _pendingContentRevision = 0;
    _nextDeadlineNanoseconds = 0;
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
    const qint64 frameIntervalNanoseconds =
        1000000000LL / _targetRefreshRate;
    const qint64 now = _clock.nsecsElapsed();
    if (_nextDeadlineNanoseconds <= 0)
        _nextDeadlineNanoseconds = now + frameIntervalNanoseconds;
    const qint64 remainingNanoseconds = std::max<qint64>(
        0, _nextDeadlineNanoseconds - now);
    // QTimer accepts integer milliseconds. Round up so the target rate is an
    // upper bound, while anchoring the deadline to the previous submission
    // prevents one extra full interval from accumulating every frame.
    const int remainingMilliseconds = qMax<int>(
        1, int((remainingNanoseconds + 999999) / 1000000));
    _timer.start(remainingMilliseconds);
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
    const qint64 now = _clock.nsecsElapsed();
    const qint64 frameIntervalNanoseconds =
        1000000000LL / _targetRefreshRate;
    // Preserve the fractional-millisecond phase when only timer rounding made
    // this submission late. If an entire interval was missed, skip it and
    // establish a new deadline instead of emitting catch-up frames.
    if (_nextDeadlineNanoseconds <= 0
        || now - _nextDeadlineNanoseconds >= frameIntervalNanoseconds) {
        _nextDeadlineNanoseconds = now + frameIntervalNanoseconds;
    } else {
        _nextDeadlineNanoseconds += frameIntervalNanoseconds;
    }
    if (_fullFramePending)
        ++_statistics.fullFrames;
    _statistics.dirtyRegionsSubmitted += quint64(_pending.size());

    const QVector<DirtyRegion> regions = std::move(_pending);
    _pending.clear();
    const bool fullFrame = std::exchange(_fullFramePending, false);
    const bool overlayDirty = std::exchange(_overlayPending, false);
    const quint64 contentRevision = std::exchange(_pendingContentRevision, 0);
    emit frameRequested(regions, fullFrame, overlayDirty, contentRevision);
}

} // namespace NovaTerm
