/**
 * @file ResourcePrefetch.h
 * @brief 绑定 SSH 连接的详情分批预取与增量缓存。
 */
#pragma once

#include "LinuxResourceData.h"
#include "ResourcePrefetchSchedule.h"
#include "transport/SshTransport.h"

#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <limits>

namespace NovaTerm::LinuxResource {

/** @brief 仅持有连接和数据；不依赖面板/对话框，切换标签不重启静态采集。 */
class ResourcePrefetch final : public QObject
{
public:
    [[nodiscard]] static ResourcePrefetch* forTransport(SshTransport* transport)
    {
        for (auto* child : transport->children()) {
            if (auto* existing = dynamic_cast<ResourcePrefetch*>(child)) {
                existing->synchronize();
                return existing;
            }
        }
        return new ResourcePrefetch(transport);
    }

    [[nodiscard]] const QByteArray& data() const noexcept { return _data; }
    [[nodiscard]] bool complete() const noexcept { return _schedule.complete(); }
    /** @brief 首次文件系统查询可先行；随后让详情预取用完各自时间片。 */
    [[nodiscard]] bool allowsSlowQuery() const
    {
        return _schedule.complete() || (_clock.isValid() && _schedule.batch() == 0
            && !_schedule.inFlight() && _clock.elapsed() < PrefetchSchedule::InitialDelayMs);
    }

private:
    explicit ResourcePrefetch(SshTransport* transport)
        : QObject(transport), _transport(transport)
    {
        _timer.setSingleShot(true);
        _timer.setTimerType(Qt::PreciseTimer);
        connect(&_timer, &QTimer::timeout, this, [this] { requestNext(); });
        connect(transport, &ITransport::connected, this, [this] { synchronize(); });
        connect(transport, &ITransport::disconnected, this, [this] { synchronize(); });
        connect(transport, &SshTransport::commandFinished, this,
                [this](quint64 id, const QByteArray& output, const QByteArray&,
                       const QString& error) {
            if (id == 0 || id != _requestId)
                return;
            if (!_transport->isConnected()
                || _generation != _transport->connectionGeneration()) {
                synchronize();
                return;
            }
            _requestId = 0;
            // 每批至多查询一次；缺命令、超时等只影响该批，不覆盖已取得的字段。
            if (error.isEmpty() && output.contains("@@done"))
                _data += output + '\n';
            _schedule.finished(_clock.elapsed());
            armTimer();
        });
        synchronize();
    }

    void synchronize()
    {
        const auto generation = _transport->connectionGeneration();
        if (!_transport->isConnected() || generation != _generation || !_clock.isValid()) {
            _timer.stop();
            _generation = generation;
            _requestId = 0;
            _schedule = {};
            _data.clear();
            _clock.invalidate();
            if (_transport->isConnected()) {
                _clock.start();
                armTimer();
            }
        }
    }

    void armTimer()
    {
        if (!_schedule.complete() && !_schedule.inFlight() && _clock.isValid())
            _timer.start(static_cast<int>(_schedule.delay(_clock.elapsed())));
    }

    void requestNext()
    {
        if (!_transport->isConnected() || _generation != _transport->connectionGeneration()) {
            synchronize();
            return;
        }
        if (!_schedule.ready(_clock.elapsed())) {
            armTimer();
            return;
        }
        static std::atomic<quint64> nextId{quint64{1} << 61};
        const quint64 id = nextId.fetch_add(1, std::memory_order_relaxed);
        if (_transport->executeCommand(id, staticCommand(_schedule.batch()))) {
            _requestId = id;
            _schedule.started(_clock.elapsed());
        } else {
            // 通道繁忙时只重试提交，不排队、不并发创建额外远端命令。
            _timer.start(100);
        }
    }

    SshTransport* _transport;
    QTimer _timer;
    QElapsedTimer _clock;
    PrefetchSchedule _schedule;
    quint64 _generation{std::numeric_limits<quint64>::max()};
    quint64 _requestId{0};
    QByteArray _data;
};

} // namespace NovaTerm::LinuxResource
