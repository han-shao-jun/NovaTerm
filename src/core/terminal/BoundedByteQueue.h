/**
 * @file   BoundedByteQueue.h
 * @brief  有界环形字节队列（生产者-消费者模型）。
 *
 * 用于在传输层（生产者）与 VT 解析工作线程（消费者）之间缓冲原始字节。
 * 队列满时阻塞生产者并触发背压；队列空时阻塞消费者。所有公开方法均
 * 线程安全，通过 std::mutex + std::condition_variable 实现等待/唤醒。
 */
#pragma once

#include "core/CoreTypes.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace NovaTerm {

// 有界环形字节队列。单生产者-单消费者语义，跨线程使用安全。
class BoundedByteQueue
{
public:
    // 队列统计快照，用于诊断背压与吞吐。
    struct Statistics
    {
        isize capacity{0};       // 队列总容量（字节）
        isize queuedBytes{0};    // 当前已入队字节数
        isize highWatermark{0};  // 历史最高水位（字节）
        uint64_t totalEnqueued{0};  // 累计入队字节
        uint64_t totalDequeued{0};  // 累计出队字节
        uint64_t producerWaits{0};  // 生产者因队列满而等待的次数
    };

    /**
     * @brief 构造有界字节队列。
     * @param capacityBytes 队列容量，默认 8 MiB。
     */
    explicit BoundedByteQueue(isize capacityBytes = 8 * 1024 * 1024);

    /**
     * @brief 入队字节序列（生产者接口）。
     * @param data 待入队字节视图。
     * @param timeoutMs 等待队列可写的超时（毫秒），-1 表示无限等待。
     * @param queuedBytesAfter 输出参数：入队完成后队列中的字节数。
     * @return true 表示全部入队成功；false 表示超时、stop() 被调用或单次入队超过容量。
     */
    bool enqueue(ByteView data, int timeoutMs = -1,
                 isize* queuedBytesAfter = nullptr);

    /**
     * @brief 出队最多 maxBytes 字节到调用方缓冲（消费者接口）。
     * @param destination 目标缓冲，至少可容纳 maxBytes 字节。
     * @param maxBytes 最多取出的字节数。
     * @param timeoutMs 等待队列非空的超时（毫秒），-1 表示无限等待。
     * @param queuedBytesAfter 输出参数：取出后队列剩余字节数（仅当返回值 > 0
     *        时有意义）。消费者据此判断低水位，无需再锁一次 statistics()。
     * @return 实际取出的字节数；超时或 stop() 后返回 0。
     * @note  由调用方提供缓冲，避免每次出队分配（对比旧版返回 QByteArray）。
     */
    isize take(char* destination, isize maxBytes, int timeoutMs = -1,
               isize* queuedBytesAfter = nullptr);

    /**
     * @brief 唤醒所有等待方并标记队列已停止。
     * 后续的 enqueue / take 调用将立即返回失败。
     */
    void stop();

    bool isEmpty() const;
    Statistics statistics() const;

private:
    isize writableBytes() const;
    // 环形写入：当尾部到数组末尾时回绕到开头。
    void copyIntoRing(const char* source, isize length);
    // 环形读取：当头部到数组末尾时回绕到开头。
    void copyFromRing(char* destination, isize length);

    mutable std::mutex _mutex;
    std::condition_variable _notEmpty;  // 队列由空变为非空时唤醒消费者
    std::condition_variable _notFull;   // 队列由满变为非满时唤醒生产者
    std::vector<char> _storage;  // 固定容量后端存储
    isize _head{0};        // 下一个出队位置（消费者读指针）
    isize _tail{0};        // 下一个入队位置（生产者写指针）
    isize _size{0};        // 当前队列有效字节数
    isize _highWatermark{0};
    uint64_t _totalEnqueued{0};
    uint64_t _totalDequeued{0};
    uint64_t _producerWaits{0};
    bool _stopped{false};
};

} // namespace NovaTerm
