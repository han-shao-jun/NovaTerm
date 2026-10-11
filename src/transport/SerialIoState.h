/**
 * @file SerialIoState.h
 * @brief 串口 I/O 与 GUI 的有界交接、代际失效及清写屏障。
 */
#pragma once
#include "ITransport.h"
#include <QByteArrayView>
#include <mutex>
#include <deque>
#include <optional>
#include <algorithm>
#include <utility>

namespace NovaTerm {

/** @brief 所有计数在同一把锁下读取；不包含任何端口或 UI 指针。 */
struct SerialIoSnapshot
{
    quint64 generation{0}, writeEpoch{0};
    bool desiredOpen{false}, stopping{false}, connected{false}, closePending{false};
    bool deliveryPaused{false}, clearPending{false};
    bool workerRunning{false}, driverCapacityQueriedBefore{false};
    qint64 receiveBytes{0}, queuedWriteBytes{0}, inFlightWriteBytes{0};
    quint64 receivedBytes{0}, deliveredBytes{0}, completedWriteBytes{0};
    quint64 receiveHighWater{0}, discardedReceiveBytes{0}, receiveFullCount{0};
    quint32 driverRxRequested{262144}, driverRxBefore{0}, driverRxActual{0};
    quint32 driverSetupError{0}, driverQueryError{0};
    bool driverRequestAccepted{false}, driverCapacityQueried{false};
};

struct SerialIoWriteBlock
{
    quint64 generation{0}, epoch{0};
    QByteArray bytes;
};

struct SerialIoNotices
{
    quint64 generation{0};
    bool opened{false}, closed{false}, writable{false};
    quint64 writeEpoch{0};
    std::optional<TransportError> error;
    QString warning;
    qint64 written{0};
};

/** @brief 端口单写者与 GUI 单消费者共用的线程安全有界状态。 */
class SerialIoState
{
public:
    static constexpr qint64 MaxReceiveBytes = 8 * 1024 * 1024;
    static constexpr qint64 MaxWriteBytes = 1024 * 1024;
    static constexpr qint64 ChunkBytes = 64 * 1024;
    explicit SerialIoState(qint64 receiveLimit = MaxReceiveBytes,
                           qint64 writeLimit = MaxWriteBytes)
        : _receiveLimit(receiveLimit), _writeLimit(writeLimit) {}
    quint64 requestOpen()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_state.stopping) return 0;
        resetConnection();
        _state.desiredOpen = true;
        _state.closePending = true;
        _state.deliveryPaused = false;
        return _state.generation;
    }
    quint64 requestClose()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        resetConnection();
        _state.desiredOpen = false;
        _state.closePending = true;
        return _state.generation;
    }
    void requestStop()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        resetConnection();
        _state.desiredOpen = false;
        _state.stopping = true;
        _state.closePending = true;
    }
    bool opened(quint64 generation)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(generation) || !_state.desiredOpen) return false;
        _state.connected = true;
        _state.closePending = false;
        _notices.opened = true;
        return true;
    }
    bool closed(quint64 generation)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (generation != _state.generation) return false;
        clearBuffers();
        _state.connected = false;
        _state.desiredOpen = false;
        _state.closePending = false;
        _notices.opened = false;
        _notices.closed = true;
        return true;
    }
    SerialIoSnapshot snapshot() const
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _state;
    }
    qint64 receiveRoom(quint64 generation) const
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        return current(generation) && _state.connected
            ? _receiveLimit - _state.receiveBytes : 0;
    }
    bool receive(quint64 generation, QByteArray bytes)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(generation) || !_state.connected) {
            _state.discardedReceiveBytes += quint64(bytes.size());
            return false;
        }
        if (bytes.size() > _receiveLimit - _state.receiveBytes) return false;
        if (bytes.isEmpty()) return true;
        _state.receiveBytes += bytes.size();
        _state.receivedBytes += quint64(bytes.size());
        _state.receiveHighWater = std::max(_state.receiveHighWater, quint64(_state.receiveBytes));
        if (_state.receiveBytes == _receiveLimit) ++_state.receiveFullCount;
        appendSegments(_received, bytes);
        return true;
    }
    QByteArray takeReceived(qint64 limit = ChunkBytes)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!_state.connected || _state.deliveryPaused || limit <= 0) return {};
        QByteArray bytes;
        while (!_received.empty() && bytes.size() < limit) {
            auto& front = _received.front();
            const auto count = std::min<qint64>(front.bytes.size() - front.offset, limit - bytes.size());
            bytes.append(front.bytes.constData() + front.offset, qsizetype(count));
            front.offset += qsizetype(count);
            if (front.offset == front.bytes.size()) _received.pop_front();
        }
        _state.receiveBytes -= bytes.size();
        _state.deliveredBytes += quint64(bytes.size());
        return bytes;
    }
    void setDeliveryPaused(bool paused)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _state.deliveryPaused = paused;
    }
    bool armReceiveNotification()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        return armReceive();
    }
    bool finishReceiveNotification(bool allowReschedule = true)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _receiveNotified = false;
        return allowReschedule && armReceive();
    }
    qint64 queueWrite(QByteArrayView bytes, qint64 limit = MaxWriteBytes)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!_state.connected || !_state.desiredOpen || _state.stopping) return -1;
        const auto available = std::min(_writeLimit, std::max<qint64>(0, limit))
            - _state.queuedWriteBytes - _state.inFlightWriteBytes;
        const auto count = std::min<qint64>(bytes.size(), std::max<qint64>(0, available));
        if (count > 0) {
            appendSegments(_writes, QByteArray(bytes.data(), qsizetype(count)));
            _state.queuedWriteBytes += count;
        }
        return count;
    }
    SerialIoWriteBlock takeWrite(quint64 generation, qint64 limit)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(generation) || !_state.connected || _state.clearPending
            || _writes.empty() || limit <= 0) return {};
        SerialIoWriteBlock block{generation, _state.writeEpoch, {}};
        auto& front = _writes.front();
        const auto count = std::min<qint64>(front.bytes.size() - front.offset, limit);
        block.bytes = front.bytes.sliced(front.offset, qsizetype(count));
        front.offset += qsizetype(count);
        if (front.offset == front.bytes.size()) _writes.pop_front();
        _state.queuedWriteBytes -= count;
        _state.inFlightWriteBytes += count;
        return block;
    }
    void returnUnwritten(const SerialIoWriteBlock& block, qint64 accepted)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(block.generation) || block.epoch != _state.writeEpoch) return;
        const auto remaining = block.bytes.size() - accepted;
        if (remaining <= 0) return;
        _state.inFlightWriteBytes -= remaining;
        _state.queuedWriteBytes += remaining;
        _writes.push_front({block.bytes.sliced(qsizetype(accepted)), 0});
    }
    qint64 completeWrite(quint64 generation, quint64 epoch, qint64 bytes)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(generation) || epoch != _state.writeEpoch || bytes <= 0) return 0;
        const auto completed = std::min(bytes, _state.inFlightWriteBytes);
        _state.inFlightWriteBytes -= completed;
        _state.completedWriteBytes += quint64(completed);
        _notices.written += completed;
        return completed;
    }
    bool requestClear()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!_state.connected || _state.stopping) return false;
        ++_state.writeEpoch;
        _writes.clear();
        _state.queuedWriteBytes = 0;
        _state.clearPending = true;
        _notices.written = 0;
        return true;
    }
    bool completeClear(quint64 generation, quint64 epoch)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(generation) || epoch != _state.writeEpoch || !_state.clearPending) return false;
        _state.inFlightWriteBytes = 0;
        _state.clearPending = false;
        _notices.writable = true;
        return true;
    }
    qint64 pendingWriteBytes() const
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        return _state.queuedWriteBytes + _state.inFlightWriteBytes
            + (_state.clearPending ? 1 : 0);
    }
    bool armWorkerWake()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_workerWakePosted) return false;
        _workerWakePosted = true;
        return true;
    }
    void consumeWorkerWake()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _workerWakePosted = false;
    }
    bool armNoticeNotification()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (_noticeNotified || (!_notices.opened && !_notices.closed
            && !_notices.writable && !_notices.error && _notices.warning.isEmpty()
            && _notices.written == 0)) return false;
        _noticeNotified = true;
        return true;
    }
    SerialIoNotices takeNotices()
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _noticeNotified = false;
        auto notices = std::move(_notices);
        notices.writeEpoch = _state.writeEpoch;
        _notices = {};
        _notices.generation = _state.generation;
        return notices;
    }
    void reportError(quint64 generation, TransportError error)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (current(generation)) _notices.error = std::move(error);
    }
    void reportWarning(quint64 generation, QString warning)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (current(generation)) _notices.warning = std::move(warning);
    }
    void setDriverQueue(quint64 generation, const SerialIoSnapshot& result)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!current(generation)) return;
        _state.driverRxBefore = result.driverRxBefore;
        _state.driverRxActual = result.driverRxActual;
        _state.driverSetupError = result.driverSetupError;
        _state.driverQueryError = result.driverQueryError;
        _state.driverRequestAccepted = result.driverRequestAccepted;
        _state.driverCapacityQueried = result.driverCapacityQueried;
        _state.driverCapacityQueriedBefore = result.driverCapacityQueriedBefore;
    }
    void setDriverQueueRequest(quint32 bytes)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (!_state.desiredOpen) _state.driverRxRequested = bytes;
    }
    void setWorkerRunning(bool running)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        _state.workerRunning = running;
    }
private:
    struct Segment { QByteArray bytes; qsizetype offset{0}; };
    static void appendSegments(std::deque<Segment>& segments, const QByteArray& bytes)
    {
        qsizetype offset = 0;
        while (offset < bytes.size()) {
            if (segments.empty() || segments.back().bytes.size() == ChunkBytes)
                segments.push_back({{}, 0});
            auto& back = segments.back().bytes;
            const auto count = std::min<qsizetype>(bytes.size() - offset, ChunkBytes - back.size());
            back.append(bytes.constData() + offset, count);
            offset += count;
        }
    }
    bool current(quint64 generation) const
    {
        return !_state.stopping && generation == _state.generation;
    }
    bool armReceive()
    {
        if (_receiveNotified || !_state.connected || _state.deliveryPaused
            || _state.receiveBytes == 0 || _state.stopping) return false;
        _receiveNotified = true;
        return true;
    }
    void clearBuffers()
    {
        _state.discardedReceiveBytes += quint64(_state.receiveBytes);
        _received.clear();
        _writes.clear();
        _state.receiveBytes = _state.queuedWriteBytes = _state.inFlightWriteBytes = 0;
        _state.clearPending = false;
    }
    void resetConnection()
    {
        ++_state.generation;
        ++_state.writeEpoch;
        clearBuffers();
        _state.connected = false;
        _state.driverRxBefore = _state.driverRxActual = 0;
        _notices = {};
        _notices.generation = _state.generation;
        // 已排队的唤醒和通知标志跨 generation 保留，消费者执行后才解除。
    }
    const qint64 _receiveLimit, _writeLimit;
    mutable std::mutex _mutex;
    SerialIoSnapshot _state;
    SerialIoNotices _notices;
    std::deque<Segment> _received, _writes;
    bool _receiveNotified{false}, _noticeNotified{false}, _workerWakePosted{false};
};
} // namespace NovaTerm
