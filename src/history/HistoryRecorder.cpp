#include "HistoryRecorder.h"
#include <QDateTime>
#include <chrono>
#include <algorithm>

namespace NovaTerm::Recording {
namespace { constexpr qsizetype RecordCharge = 256; constexpr qsizetype MaxPayload = 64 * 1024; }
bool QueueBudget::reserve(qsizetype bytes, qsizetype sessionUsed) {
    auto previous = _used.load();
    const auto reservedPerSession = std::min<qsizetype>(64 * 1024 + RecordCharge, _limit / 32);
    const auto reserved = 16 * reservedPerSession;
    do {
        if (bytes < 0 || bytes > _limit - previous) return false;
        // Keep a small admission allowance for each of the at most 16 active
        // captures. A few saturated sessions cannot consume the final allowance.
        if (bytes > _limit - reserved - previous && sessionUsed + bytes > reservedPerSession) return false;
    }
    while (!_used.compare_exchange_weak(previous, previous + bytes));
    return true;
}
HistoryRecorder::HistoryRecorder(HistoryCaptureId id, QUuid session, quint64 generation,
    std::shared_ptr<QueueBudget> budget, qsizetype limit)
    : _id(id), _session(session), _generation(generation), _budget(std::move(budget)),
      _limit((std::max)(RecordCharge, limit)) {}
HistoryRecorder::~HistoryRecorder() { _budget->release(_bytes); }
HistoryRecord HistoryRecorder::next(RecordKind kind) {
    HistoryRecord r; r.captureId = _id; r.sessionId = _session; r.connectionGeneration = _generation;
    r.recordId = _nextId++; r.lastRecordId = r.recordId; r.kind = kind;
    r.wallTimestampMs = QDateTime::currentMSecsSinceEpoch();
    r.monotonicTimestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return r;
}
void HistoryRecorder::drop(quint64 bytes, const QString& reason) {
    auto r = next(RecordKind::Gap);
    if (!_tailGap) {
        r.reason = reason.left(128); r.lostByteCount = bytes; r.lostRecordCount = 1;
        _tailGap = std::move(r);
    } else {
        _tailGap->lastRecordId = r.recordId;
        *_tailGap->lostByteCount += bytes;
        ++*_tailGap->lostRecordCount;
        if (_tailGap->reason != reason) _tailGap->reason = QStringLiteral("multiple_omissions");
    }
}
SubmitResult HistoryRecorder::submit(const QByteArray& bytes) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_accepting) return SubmitResult::Disabled;
    if (bytes.isEmpty()) return SubmitResult::Accepted;
    // A reserved, single tail gap cannot grow with the number of rejected inputs.
    if (_tailGap || bytes.size() > _limit - _bytes - RecordCharge) {
        drop(quint64(bytes.size()), QStringLiteral("queue_limit")); return SubmitResult::GapRecorded;
    }
    const auto pieces = (bytes.size() + MaxPayload - 1) / MaxPayload;
    const auto charge = bytes.size() + pieces * RecordCharge;
    if (charge > _limit - _bytes || !_budget->reserve(charge, _bytes)) {
        drop(quint64(bytes.size()), QStringLiteral("queue_limit")); return SubmitResult::GapRecorded;
    }
    _bytes += charge;
    for (qsizetype offset = 0; offset < bytes.size(); offset += MaxPayload) {
        auto r = next(RecordKind::Payload); r.payload = bytes.mid(offset, MaxPayload);
        _queue.push_back(std::move(r));
    }
    return SubmitResult::Accepted;
}
SubmitResult HistoryRecorder::omit(quint64 bytes, const QString& reason) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_accepting) return SubmitResult::Disabled;
    // Queue the previous gap before accepting subsequent normal bytes, if space permits.
    if (!_tailGap && _bytes + RecordCharge <= _limit && _budget->reserve(RecordCharge, _bytes)) {
        auto r = next(RecordKind::Gap); r.reason = reason.left(128);
        r.lostByteCount = bytes; r.lostRecordCount = 1;
        _queue.push_back(std::move(r)); _bytes += RecordCharge;
    } else drop(bytes, reason);
    return SubmitResult::GapRecorded;
}
void HistoryRecorder::stop(QString reason) {
    std::lock_guard<std::mutex> lock(_mutex); _accepting = false; _reason = reason.left(128);
}
void HistoryRecorder::boundary(const QString& reason) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_accepting) return;
    if (_tailGap || _bytes + RecordCharge > _limit || !_budget->reserve(RecordCharge, _bytes)) {
        drop(0, "boundary_lost"); return;
    }
    auto r = next(RecordKind::Boundary); r.reason = reason.left(128);
    _queue.push_back(std::move(r)); _bytes += RecordCharge;
}
std::vector<HistoryRecord> HistoryRecorder::takeBatch(qsizetype maxBytes) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<HistoryRecord> result; qsizetype charge = 0;
    while (!_queue.empty() && charge < maxBytes) {
        charge += _queue.front().payload.size() + RecordCharge;
        result.push_back(std::move(_queue.front())); _queue.pop_front();
    }
    _bytes -= charge; _budget->release(charge);
    if (_queue.empty() && _tailGap) { result.push_back(std::move(*_tailGap)); _tailGap.reset(); }
    return result;
}
bool HistoryRecorder::drained() const { std::lock_guard<std::mutex> lock(_mutex); return _queue.empty() && !_tailGap; }
bool HistoryRecorder::accepting() const { std::lock_guard<std::mutex> lock(_mutex); return _accepting; }
QString HistoryRecorder::stopReason() const { std::lock_guard<std::mutex> lock(_mutex); return _reason; }
qsizetype HistoryRecorder::queuedBytes() const { std::lock_guard<std::mutex> lock(_mutex); return _bytes; }
}
