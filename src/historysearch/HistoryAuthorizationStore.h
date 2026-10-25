#pragma once
#include "HistorySearchTypes.h"
#include <QMutex>
#include <map>

namespace NovaTerm::Recording {
class HistoryAuthorizationStore final {
public:
    ResourceGrant setGrant(ResourceGrant);
    void revoke(const QString& principalId, HistoryCaptureId);
    void revokePrincipal(const QString& principalId);
    std::optional<ResourceGrant> authorize(const ResourceContext&, HistoryCaptureId,
                                            quint64 generation) const;
    std::vector<ResourceGrant> grants(const ResourceContext&) const;
    std::vector<ResourceGrant> allGrants() const;
    static bool allowsRecord(const ResourceGrant&, const HistoryRecord&);
private:
    mutable QMutex _mutex;
    std::map<std::pair<QString, QString>, ResourceGrant> _grants;
    quint64 _version{0};
};
}
