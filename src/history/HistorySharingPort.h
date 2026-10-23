#pragma once
#include "HistoryAuthorizationStore.h"
namespace NovaTerm::Recording {
struct SharingClient {QString id,label;};
class HistorySharingPort {
public:
    virtual ~HistorySharingPort() = default;
    virtual std::vector<SharingClient> clients() const = 0;
    virtual std::optional<ResourceGrant> grant(const QString& client,HistoryCaptureId) const = 0;
    virtual bool share(ResourceGrant) = 0;
    virtual bool revoke(const QString& client,HistoryCaptureId) = 0;
};
}
