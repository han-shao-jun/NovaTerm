/**
 * @file   BoundedByteQueue.cpp
 * @brief  有界环形字节队列实现。
 *
 * 详见 BoundedByteQueue.h 的接口说明。本文件实现环形缓冲的拷贝/等待逻辑：
 * 当 _tail / _head 推进到 _storage 末尾时回绕到 0，从而复用已出队的存储空间。
 */
#include "BoundedByteQueue.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace NovaTerm {
namespace {

// 计算 wait 的绝对截止时刻；timeoutMs < 0 表示无限等待。
using SteadyClock = std::chrono::steady_clock;

} // namespace

BoundedByteQueue::BoundedByteQueue(isize capacityBytes)
    // std::vector 默认零初始化，容量下限 1 字节。
    : _storage(std::size_t(std::max<isize>(1, capacityBytes)))
{
}

bool BoundedByteQueue::enqueue(ByteView data, int timeoutMs,
                               isize* queuedBytesAfter)
{
    // 空数据视为成功入队，仅返回当前字节数。
    if (data.empty()) {
        if (queuedBytesAfter) {
            std::lock_guard<std::mutex> locker(_mutex);
            *queuedBytesAfter = _size;
        }
        return true;
    }
    // 单次入队超过队列总容量，永远不可能成功。
    if (data.size > isize(_storage.size()))
        return false;

    std::unique_lock<std::mutex> locker(_mutex);
    const bool timed = timeoutMs >= 0;
    const auto deadline =
        SteadyClock::now() + std::chrono::milliseconds(timed ? timeoutMs : 0);
    // 队列满时阻塞生产者，直到队列非满或被停止。超时立即返回失败，
    // 语义与原 QWaitCondition::wait 返回 false 一致。
    while (!_stopped && writableBytes() < data.size) {
        ++_producerWaits;
        if (timed) {
            if (_notFull.wait_until(locker, deadline)
                == std::cv_status::timeout) {
                if (queuedBytesAfter)
                    *queuedBytesAfter = _size;
                return false;
            }
        } else {
            _notFull.wait(locker);
        }
    }
    if (_stopped)
        return false;

    copyIntoRing(data.data, data.size);
    _size += data.size;
    _totalEnqueued += uint64_t(data.size);
    _highWatermark = std::max(_highWatermark, _size);
    if (queuedBytesAfter)
        *queuedBytesAfter = _size;
    _notEmpty.notify_one();
    return true;
}

isize BoundedByteQueue::take(char* destination, isize maxBytes, int timeoutMs,
                             isize* queuedBytesAfter)
{
    if (maxBytes <= 0 || !destination)
        return 0;

    std::unique_lock<std::mutex> locker(_mutex);
    const bool timed = timeoutMs >= 0;
    const auto deadline =
        SteadyClock::now() + std::chrono::milliseconds(timed ? timeoutMs : 0);
    // 队列空时阻塞消费者，直到非空或被停止。
    while (!_stopped && _size == 0) {
        if (timed) {
            if (_notEmpty.wait_until(locker, deadline)
                == std::cv_status::timeout) {
                return 0;
            }
        } else {
            _notEmpty.wait(locker);
        }
    }
    if (_size == 0)
        return 0;

    // 最多取出请求量与当前队列内容中的较小值。
    const isize length = std::min(maxBytes, _size);
    copyFromRing(destination, length);
    _size -= length;
    _totalDequeued += uint64_t(length);
    if (queuedBytesAfter)
        *queuedBytesAfter = _size;
    _notFull.notify_all();
    return length;
}

void BoundedByteQueue::stop()
{
    std::lock_guard<std::mutex> locker(_mutex);
    _stopped = true;
    // 唤醒所有阻塞的生产者与消费者，让它们看到 _stopped 后退出。
    _notEmpty.notify_all();
    _notFull.notify_all();
}

bool BoundedByteQueue::isEmpty() const
{
    std::lock_guard<std::mutex> locker(_mutex);
    return _size == 0;
}

BoundedByteQueue::Statistics BoundedByteQueue::statistics() const
{
    std::lock_guard<std::mutex> locker(_mutex);
    return {isize(_storage.size()), _size, _highWatermark, _totalEnqueued,
            _totalDequeued, _producerWaits};
}

isize BoundedByteQueue::writableBytes() const
{
    return isize(_storage.size()) - _size;
}

void BoundedByteQueue::copyIntoRing(const char* source, isize length)
{
    const isize capacity = isize(_storage.size());
    // 第一段：从 _tail 到数组末尾能写入的部分。
    const isize first = std::min(length, capacity - _tail);
    std::memcpy(_storage.data() + _tail, source, size_t(first));
    // 第二段：剩余部分回绕到数组开头。
    const isize second = length - first;
    if (second > 0)
        std::memcpy(_storage.data(), source + first, size_t(second));
    _tail = (_tail + length) % capacity;
}

void BoundedByteQueue::copyFromRing(char* destination, isize length)
{
    const isize capacity = isize(_storage.size());
    // 与 copyIntoRing 对称：先读 _head 到末尾，再回绕读剩余部分。
    const isize first = std::min(length, capacity - _head);
    std::memcpy(destination, _storage.data() + _head, size_t(first));
    const isize second = length - first;
    if (second > 0)
        std::memcpy(destination + first, _storage.data(), size_t(second));
    _head = (_head + length) % capacity;
}

} // namespace NovaTerm
