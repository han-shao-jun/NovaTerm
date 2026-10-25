#include "HistoryRetentionPolicy.h"
#include "HistoryStorageIo.h"
#include <QDir>
#include <algorithm>

namespace NovaTerm::Recording {
bool RetentionSettings::valid() const {
    return captureBytes >= 1024 * 1024 && captureBytes <= 16ull * 1024 * 1024 * 1024
        && globalBytes >= captureBytes && globalBytes <= 64ull * 1024 * 1024 * 1024
        && retentionMs >= 60000 && retentionMs <= 365ll * 24 * 60 * 60 * 1000;
}
void HistoryRetentionPolicy::expire(qint64 now) {
    for (auto it = _leases.begin(); it != _leases.end();) {
        if (it->second.expiresMs <= now) it = _leases.erase(it); else ++it;
    }
}
std::optional<ReadLease> HistoryRetentionPolicy::acquire(HistoryCaptureId id, qint64 now, qint64 duration) {
    expire(now); if (!id.valid() || _leases.size() >= 32 || duration <= 0) return {};
    ReadLease lease{QUuid::createUuid(), id, now + std::min<qint64>(duration, 30000), now + 30000};
    _leases.emplace(lease.token.toString(), lease); return lease;
}
bool HistoryRetentionPolicy::renew(const ReadLease& lease, qint64 now, qint64 duration) {
    expire(now); const auto it = _leases.find(lease.token.toString());
    if (it == _leases.end() || !(it->second.capture == lease.capture) || duration <= 0) return false;
    it->second.expiresMs = (std::min)(now + (std::min<qint64>)(duration, 30000), it->second.hardExpiresMs); return true;
}
void HistoryRetentionPolicy::release(const ReadLease& lease) { _leases.erase(lease.token.toString()); }
bool HistoryRetentionPolicy::leased(HistoryCaptureId id, qint64 now) {
    expire(now); for (const auto& entry : _leases) if (entry.second.capture == id) return true; return false;
}
bool HistoryRetentionPolicy::configure(RetentionSettings settings) { if (!settings.valid()) return false; _settings = settings; return true; }
bool HistoryRetentionPolicy::removeCapture(const QString& root, HistoryCaptureId id, qint64 now, bool active) {
    if (!id.valid() || active || leased(id, now) || !StorageIo::safePath(root)) return false;
    const QString directory = QDir(root).absoluteFilePath(id.text());
    return StorageIo::removePrivateTree(directory) && StorageIo::syncDirectory(root);
}
}
