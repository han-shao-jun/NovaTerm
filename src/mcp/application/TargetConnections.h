#pragma once
#include "SavedTargets.h"
#include "mcp/contracts/SavedTargetRequest.h"
#include "mcp/contracts/Errors.h"
#include <functional>

namespace NovaTerm::Mcp {
struct TargetConnectionFacts {
    QString sessionId, epoch, attachmentId, state{"unavailable"}, reason;
    bool created{false};
    quint64 connectionGeneration{0};
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
    using Close = std::function<TargetConnectionResult(const TargetConnectionFacts&, const QString&, const Guard&)>;
    explicit TargetConnections(SavedTargets& targets) : _targets(targets) {}
    void setPorts(Open open, Query query, Close close = {});
    TargetConnectionResult call(const QString& client, quint64 identityVersion, const TargetConnectionRequest&,
        Guard invocationValid = {});
private:
    struct Record {
        quint64 identityVersion;
        QString targetId, configVersion;
        TargetConnectionFacts facts;
        bool closeInFlight{false}, closeAccepted{false};
        TargetConnectionFacts closeFacts;
        QString closeEpoch;
    };
    SavedTargets& _targets;
    QMap<QString, QMap<QString, Record>> _records;
    Open _open;
    Query _query;
    Close _close;
};
}
