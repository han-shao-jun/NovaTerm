/**
 * @file   RenderScheduler.h
 * @brief  帧调度器：把高频 damage 合并为按目标刷新率节流的绘制请求。
 *
 * TerminalCore 发出的 damage 信号可能远超显示器刷新率（如 cat 大文件
 * 时每行多次 damage）。RenderScheduler 收集一帧间隔内的所有脏区域，
 * 合并相邻/重叠区域，按 60/120/144 Hz 节流后通过 frameRequested 信号
 * 通知 TerminalRenderer 真正重绘。
 */
#pragma once

#include "core/terminal/TerminalTypes.h"

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>

#include <cstdint>

namespace NovaTerm {

// 调度统计，用于诊断合并率与帧率。
struct RenderScheduleStatistics
{
    quint64 dirtyRegionsReceived{0};
    quint64 dirtyRegionsSubmitted{0};
    quint64 framesRequested{0};
    quint64 coalescedFrameRequests{0};
    quint64 fullFrames{0};
};

// 帧调度器。GUI 线程独占，无需加锁。
class RenderScheduler : public QObject
{
    Q_OBJECT

public:
    explicit RenderScheduler(QObject* parent = nullptr);

    void setViewport(int columns, int rows);
    void setTargetRefreshRate(int hz);
    int targetRefreshRate() const { return _targetRefreshRate; }

    /**
     * @brief 调度一个脏区域。合并到待发列表，按目标刷新率节流后投递。
     * @param region 脏区域。
     * @param revision 关联的模型版本号，调度器会取最大值传给 frameRequested。
     */
    void schedule(const DirtyRegion& region, quint64 revision = 0);

    /**
     * @brief 请求一次全屏重绘。
     */
    void scheduleFullFrame(quint64 revision = 0);

    /**
     * @brief 请求重绘 overlay（光标、选区等），不触发整屏重绘。
     */
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
    // 两个脏区域是否相邻或重叠（含共享一条边的情况），用于合并判定。
    static bool touches(const DirtyRegion& lhs, const DirtyRegion& rhs);
    // 取两个脏区域的最小包围盒。
    static DirtyRegion united(const DirtyRegion& lhs, const DirtyRegion& rhs);
    void armTimer();
    void mergePending();
    // 当脏区域数量或覆盖面积超过阈值时，退化为全屏重绘，避免合并开销
    // 超过直接全屏重绘的开销。
    bool shouldPromoteToFullFrame() const;

    int _columns{0};
    int _rows{0};
    int _targetRefreshRate{60};
    QElapsedTimer _clock;
    // 下一帧的目标截止时间（纳秒），用于相位对齐避免帧间隔漂移。
    qint64 _nextDeadlineNanoseconds{0};
    QTimer _timer;
    QVector<DirtyRegion> _pending;
    bool _fullFramePending{false};
    bool _overlayPending{false};
    quint64 _pendingContentRevision{0};
    RenderScheduleStatistics _statistics;
};

} // namespace NovaTerm
