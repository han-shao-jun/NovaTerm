/**
 * @file   RenderScheduler.cpp
 * @brief  帧调度器实现。
 *
 * 详见 RenderScheduler.h。本文件实现：
 * - 脏区域合并（touches/united）：相邻或重叠的矩形合并为一个，减少
 *   传给渲染器的区域数；
 * - 全屏提升（shouldPromoteToFullFrame）：当脏区域数量或覆盖面积超过
 *   阈值时退化为全屏重绘；
 * - 相位对齐的节流（armTimer/submitFrame）：deadline 锚定到上次提交，
 *   避免每帧累积一个完整间隔的漂移。
 */
#include "RenderScheduler.h"

#include <algorithm>
#include <utility>

namespace NovaTerm {

namespace {
// 待合并脏区域数量上限，超过即提升为全屏重绘。
constexpr int MaxDirtyRegions = 32;
// 脏面积占屏幕比例阈值，超过即提升为全屏重绘。
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
    // 视口尺寸变化意味着行/列布局改变，必须全屏重绘。
    scheduleFullFrame();
}

void RenderScheduler::setTargetRefreshRate(int hz)
{
    // 仅支持常见刷新率，其余按 60 Hz 处理。
    if (hz != 60 && hz != 120 && hz != 144)
        hz = 60;
    if (_targetRefreshRate == hz)
        return;
    _targetRefreshRate = hz;
    // 重置 deadline 以新刷新率重新对齐相位。
    _nextDeadlineNanoseconds = 0;
}

void RenderScheduler::schedule(const DirtyRegion& region, quint64 revision)
{
    if (_columns <= 0 || _rows <= 0)
        return;

    // 把脏区域裁剪到视口内，避免渲染器访问越界行/列。
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
    // 已有全屏帧待发：无需再记录脏区域，全屏会覆盖它们。
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

// 两个脏区域是否相邻或重叠。除常规重叠外，"共享一条边"也算相邻：
// 终端中相邻行/列的更新往往源于同一次操作（如整行重写），合并后
// 可减少一次 draw call。
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
    // QTimer 只接受整数毫秒。向上取整保证目标刷新率是上界；
    // 同时把 deadline 锚定到上次提交而非"现在"，避免每帧多累积一个
    // 完整间隔（否则实际帧率会低于目标）。
    const int remainingMilliseconds = qMax<int>(
        1, int((remainingNanoseconds + 999999) / 1000000));
    _timer.start(remainingMilliseconds);
}

// 反复合并相邻/重叠区域，直到无可合并为止。O(n^2) 但 n 受
// MaxDirtyRegions 上限保护，实际很小。
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
    // 仅由 QTimer 毫秒取整导致的小幅迟到，保留亚毫秒相位继续累积；
    // 若已错过整整一个间隔，则跳过补帧并重新建立 deadline，避免
    // 雪崩式补帧把渲染器压垮。
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
