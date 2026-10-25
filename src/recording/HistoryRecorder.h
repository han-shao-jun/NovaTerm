#pragma once
#include "HistoryTypes.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace NovaTerm::Recording {
class QueueBudget final {
public:
    explicit QueueBudget(qsizetype limit = 64 * 1024 * 1024) : _limit(limit) {}
    bool reserve(qsizetype bytes, qsizetype sessionUsed = 0);
    void release(qsizetype bytes) { _used.fetch_sub(bytes); }
    qsizetype used() const { return _used.load(); }
private:
    const qsizetype _limit;
    std::atomic<qsizetype> _used{0};
};
// One producer (the Session thread), one worker consumer. No disk I/O under this mutex.
class HistoryRecorder final {
public:
    HistoryRecorder(HistoryCaptureId, QUuid session, quint64 generation,
                    std::shared_ptr<QueueBudget>, qsizetype limit = 16 * 1024 * 1024);
    ~HistoryRecorder();
    SubmitResult submit(const QByteArray&);
    SubmitResult omit(quint64 bytes, const QString& reason);
    void boundary(const QString& reason);
    void stop(QString reason);
    std::vector<HistoryRecord> takeBatch(qsizetype maxBytes = 256 * 1024);
    bool drained() const;
    bool accepting() const;
    QString stopReason() const;
    qsizetype queuedBytes() const;
private:
    HistoryRecord next(RecordKind);
    void drop(quint64 bytes, const QString& reason);
    const HistoryCaptureId _id;
    const QUuid _session;
    const quint64 _generation;
    const std::shared_ptr<QueueBudget> _budget;
    const qsizetype _limit;
    mutable std::mutex _mutex;
    std::deque<HistoryRecord> _queue;
    std::optional<HistoryRecord> _tailGap;
    quint64 _nextId{1};
    qsizetype _bytes{0};
    bool _accepting{true};
    QString _reason;
};
}
