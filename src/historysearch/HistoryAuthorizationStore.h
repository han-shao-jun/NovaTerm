#pragma once
#include "HistorySearchTypes.h"
#include <QMutex>
#include <map>
#include <utility>

namespace NovaTerm::Recording {
class HistoryAuthorizationStore final {
public:
    // Optional owner-thread application policy. The explicit grant store remains
    // available to callers that need bounded record or timestamp authorization.
    using Resolver = std::function<std::optional<ResourceGrant>(const ResourceContext&, HistoryCaptureId, quint64)>;
    explicit HistoryAuthorizationStore(Resolver resolver = {}) : _resolver(std::move(resolver)) {}
    ResourceGrant setGrant(ResourceGrant);
    void revoke(const QString& principalId, HistoryCaptureId);
    void revokePrincipal(const QString& principalId);
    std::optional<ResourceGrant> authorize(const ResourceContext&, HistoryCaptureId,
                                            quint64 generation) const;
    std::vector<ResourceGrant> grants(const ResourceContext&) const;
    std::vector<ResourceGrant> allGrants() const;
    static bool allowsRecord(const ResourceGrant&, const HistoryRecord&);
private:
    Resolver _resolver;
    mutable QMutex _mutex;
    std::map<std::pair<QString, QString>, ResourceGrant> _grants;
    quint64 _version{0};
};
}
