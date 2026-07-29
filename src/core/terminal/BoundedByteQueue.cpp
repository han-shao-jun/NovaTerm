#include "BoundedByteQueue.h"

#include <QDeadlineTimer>
#include <QMutexLocker>

#include <algorithm>
#include <cstring>

namespace NovaTerm {

BoundedByteQueue::BoundedByteQueue(qsizetype capacityBytes)
    : _storage(std::max<qsizetype>(1, capacityBytes), Qt::Uninitialized)
{
}

bool BoundedByteQueue::enqueue(const QByteArray& data, int timeoutMs)
{
    if (data.isEmpty())
        return true;
    if (data.size() > _storage.size())
        return false;

    QMutexLocker locker(&_mutex);
    QDeadlineTimer deadline(timeoutMs < 0 ? QDeadlineTimer::Forever
                                         : QDeadlineTimer(timeoutMs));
    while (!_stopped && writableBytes() < data.size()) {
        ++_producerWaits;
        if (!_notFull.wait(&_mutex, deadline))
            return false;
    }
    if (_stopped)
        return false;

    copyIntoRing(data.constData(), data.size());
    _size += data.size();
    _totalEnqueued += uint64_t(data.size());
    _highWatermark = std::max(_highWatermark, _size);
    _notEmpty.wakeOne();
    return true;
}

QByteArray BoundedByteQueue::take(qsizetype maxBytes, int timeoutMs)
{
    if (maxBytes <= 0)
        return {};

    QMutexLocker locker(&_mutex);
    QDeadlineTimer deadline(timeoutMs < 0 ? QDeadlineTimer::Forever
                                         : QDeadlineTimer(timeoutMs));
    while (!_stopped && _size == 0) {
        if (!_notEmpty.wait(&_mutex, deadline))
            return {};
    }
    if (_size == 0)
        return {};

    const qsizetype length = std::min(maxBytes, _size);
    QByteArray result(length, Qt::Uninitialized);
    copyFromRing(result.data(), length);
    _size -= length;
    _totalDequeued += uint64_t(length);
    _notFull.wakeAll();
    return result;
}

void BoundedByteQueue::stop()
{
    QMutexLocker locker(&_mutex);
    _stopped = true;
    _notEmpty.wakeAll();
    _notFull.wakeAll();
}

bool BoundedByteQueue::isEmpty() const
{
    QMutexLocker locker(&_mutex);
    return _size == 0;
}

BoundedByteQueue::Statistics BoundedByteQueue::statistics() const
{
    QMutexLocker locker(&_mutex);
    return {_storage.size(), _size, _highWatermark, _totalEnqueued,
            _totalDequeued, _producerWaits};
}

qsizetype BoundedByteQueue::writableBytes() const
{
    return _storage.size() - _size;
}

void BoundedByteQueue::copyIntoRing(const char* source, qsizetype length)
{
    const qsizetype first = std::min(length, _storage.size() - _tail);
    std::memcpy(_storage.data() + _tail, source, size_t(first));
    const qsizetype second = length - first;
    if (second > 0)
        std::memcpy(_storage.data(), source + first, size_t(second));
    _tail = (_tail + length) % _storage.size();
}

void BoundedByteQueue::copyFromRing(char* destination, qsizetype length)
{
    const qsizetype first = std::min(length, _storage.size() - _head);
    std::memcpy(destination, _storage.constData() + _head, size_t(first));
    const qsizetype second = length - first;
    if (second > 0)
        std::memcpy(destination + first, _storage.constData(), size_t(second));
    _head = (_head + length) % _storage.size();
}

} // namespace NovaTerm
