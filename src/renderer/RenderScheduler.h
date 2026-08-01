#pragma once

#include "core/terminal/TerminalTypes.h"

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>

#include <cstdint>

namespace NovaTerm {

struct RenderScheduleStatistics
{
    quint64 dirtyRegionsReceived{0};
    quint64 dirtyRegionsSubmitted{0};
    quint64 framesRequested{0};
    quint64 coalescedFrameRequests{0};
    quint64 fullFrames{0};
};

class RenderScheduler : public QObject
{
    Q_OBJECT

public:
    explicit RenderScheduler(QObject* parent = nullptr);

    void setViewport(int columns, int rows);
    void setTargetRefreshRate(int hz);
    int targetRefreshRate() const { return _targetRefreshRate; }

    void schedule(const DirtyRegion& region, quint64 revision = 0);
    void scheduleFullFrame(quint64 revision = 0);
    void scheduleOverlay();
    void cancel();

    const RenderScheduleStatistics& statistics() const { return _statistics; }

signals:
    void frameRequested(const QVector<NovaTerm::DirtyRegion>& regions,
                        bool fullFrame,
                        bool overlayDirty,
                        quint64 contentRevision);

private slots:
    void submitFrame();

private:
    static bool touches(const DirtyRegion& lhs, const DirtyRegion& rhs);
    static DirtyRegion united(const DirtyRegion& lhs, const DirtyRegion& rhs);
    void armTimer();
    void mergePending();
    bool shouldPromoteToFullFrame() const;

    int _columns{0};
    int _rows{0};
    int _targetRefreshRate{60};
    QElapsedTimer _clock;
    qint64 _nextDeadlineNanoseconds{0};
    QTimer _timer;
    QVector<DirtyRegion> _pending;
    bool _fullFramePending{false};
    bool _overlayPending{false};
    quint64 _pendingContentRevision{0};
    RenderScheduleStatistics _statistics;
};

} // namespace NovaTerm
