#pragma once
#include "HistoryTypes.h"
#include <map>

namespace NovaTerm::Recording {
struct RetentionSettings {
    quint64 globalBytes{2ull * 1024 * 1024 * 1024};
    quint64 captureBytes{512ull * 1024 * 1024};
    qint64 retentionMs{7ll * 24 * 60 * 60 * 1000};
    bool valid() const;
};
struct ReadLease { QUuid token; HistoryCaptureId capture; qint64 expiresMs{0}; qint64 hardExpiresMs{0}; };
class HistoryRetentionPolicy final {
public:
    explicit HistoryRetentionPolicy(RetentionSettings settings = {}) : _settings(settings) {}
    std::optional<ReadLease> acquire(HistoryCaptureId, qint64 nowMs, qint64 durationMs = 30000);
    bool renew(const ReadLease&, qint64 nowMs, qint64 durationMs = 30000);
    void release(const ReadLease&);
    bool leased(HistoryCaptureId, qint64 nowMs);
    RetentionSettings settings() const { return _settings; }
    bool configure(RetentionSettings settings);
    // Called only by History's worker, after stores are closed. Removes all derived files too.
    bool removeCapture(const QString& root, HistoryCaptureId, qint64 nowMs, bool active);
private:
    void expire(qint64 nowMs);
    RetentionSettings _settings;
    std::map<QString, ReadLease> _leases;
};
}
