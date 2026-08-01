#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QMutex>
#include <QWaitCondition>

#include <cstdint>

namespace NovaTerm {

class BoundedByteQueue
{
public:
    struct Statistics
    {
        qsizetype capacity{0};
        qsizetype queuedBytes{0};
        qsizetype highWatermark{0};
        uint64_t totalEnqueued{0};
        uint64_t totalDequeued{0};
        uint64_t producerWaits{0};
    };

    explicit BoundedByteQueue(qsizetype capacityBytes = 8 * 1024 * 1024);

    bool enqueue(QByteArrayView data, int timeoutMs = -1,
                 qsizetype* queuedBytesAfter = nullptr);
    QByteArray take(qsizetype maxBytes, int timeoutMs = -1);
    void stop();

    bool isEmpty() const;
    Statistics statistics() const;

private:
    qsizetype writableBytes() const;
    void copyIntoRing(const char* source, qsizetype length);
    void copyFromRing(char* destination, qsizetype length);

    mutable QMutex _mutex;
    QWaitCondition _notEmpty;
    QWaitCondition _notFull;
    QByteArray _storage;
    qsizetype _head{0};
    qsizetype _tail{0};
    qsizetype _size{0};
    qsizetype _highWatermark{0};
    uint64_t _totalEnqueued{0};
    uint64_t _totalDequeued{0};
    uint64_t _producerWaits{0};
    bool _stopped{false};
};

} // namespace NovaTerm
