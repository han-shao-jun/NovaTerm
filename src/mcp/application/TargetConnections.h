#pragma once
#include "SavedTargets.h"
#include "mcp/contracts/SavedTargetRequest.h"
#include "mcp/contracts/Errors.h"
#include <functional>

namespace NovaTerm::Mcp {
struct TargetConnectionFacts {
    QString sessionId, epoch, attachmentId, state{"unavailable"}, reason;
    bool created{false};
};
struct TargetConnectionResult {
    TargetConnectionFacts facts;
    std::optional<ErrorCode> error;
};
// Application-thread admission index. Visible views and their sessions remain UI-owned.
class TargetConnections final {
public:
    using Guard = std::function<bool()>;
    using Open = std::function<TargetConnectionFacts(const SavedTargetSource&, const Guard&)>;
    using Query = std::function<TargetConnectionFacts(const QString&, const QString&)>;
    explicit TargetConnections(SavedTargets& targets) : _targets(targets) {}
    void setPorts(Open open, Query query);
    TargetConnectionResult call(const QString& client, quint64 identityVersion, const TargetConnectionRequest&,
        Guard invocationValid = {});
private:
    struct Record {
        quint64 identityVersion;
        QString targetId, configVersion;
        TargetConnectionFacts facts;
    };
    SavedTargets& _targets;
    QMap<QString, QMap<QString, Record>> _records;
    Open _open;
    Query _query;
};
}
