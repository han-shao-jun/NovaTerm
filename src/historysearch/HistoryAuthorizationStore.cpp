#include "HistoryAuthorizationStore.h"
#include <QMutexLocker>
namespace NovaTerm::Recording {
ResourceGrant HistoryAuthorizationStore::setGrant(ResourceGrant g) {
    QMutexLocker lock(&_mutex);
    if (g.principalId.isEmpty() || !g.captureId.valid() || !g.resourceGeneration
        || !g.firstRecordId || (g.lastRecordId && g.lastRecordId < g.firstRecordId)
        || (g.firstTimestampMs && g.lastTimestampMs && *g.firstTimestampMs > *g.lastTimestampMs)) {
        g.accessVersion = 0; return g;
    }
    g.accessVersion = ++_version;
    _grants[{g.principalId, g.captureId.text()}] = g;
    return g;
}
void HistoryAuthorizationStore::revoke(const QString& principal, HistoryCaptureId id) {
    QMutexLocker lock(&_mutex); _grants.erase({principal, id.text()}); ++_version;
}
void HistoryAuthorizationStore::revokePrincipal(const QString& principal) {
    QMutexLocker lock(&_mutex);
    for (auto i = _grants.begin(); i != _grants.end();) {
        if (i->second.principalId == principal) i = _grants.erase(i); else ++i;
    }
    ++_version;
}
std::optional<ResourceGrant> HistoryAuthorizationStore::authorize(const ResourceContext& context, HistoryCaptureId id, quint64 generation) const {
    if (_resolver) return _resolver(context, id, generation);
    QMutexLocker lock(&_mutex); const auto i = _grants.find({context.principalId, id.text()});
    if (i == _grants.end() || i->second.tokenEpoch != context.tokenEpoch
        || i->second.resourceGeneration != generation) return {};
    return i->second;
}
std::vector<ResourceGrant> HistoryAuthorizationStore::grants(const ResourceContext& context) const {
    QMutexLocker lock(&_mutex); std::vector<ResourceGrant> result;
    for (const auto& entry : _grants)
        if (entry.second.principalId == context.principalId && entry.second.tokenEpoch == context.tokenEpoch)
            result.push_back(entry.second);
    return result;
}
bool HistoryAuthorizationStore::allowsRecord(const ResourceGrant& grant, const HistoryRecord& record) {
    const auto last = record.lastRecordId ? record.lastRecordId : record.recordId;
    return record.recordId >= grant.firstRecordId && (!grant.lastRecordId || last <= grant.lastRecordId)
        && (!grant.firstTimestampMs || record.wallTimestampMs >= *grant.firstTimestampMs)
        && (!grant.lastTimestampMs || record.wallTimestampMs <= *grant.lastTimestampMs);
}
std::vector<ResourceGrant> HistoryAuthorizationStore::allGrants() const {
    QMutexLocker lock(&_mutex);
    std::vector<ResourceGrant> result;
    for (const auto& entry : _grants) result.push_back(entry.second);
    return result;
}
}
