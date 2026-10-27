#pragma once
#include "McpAccess.h"
#include "application/TargetConnections.h"
#include <QDeadlineTimer>
#include <QTimer>

namespace NovaTerm::Mcp {
// Session-thread adapter for one-shot local rules; sessions/transports remain view-owned.
class TargetSessionSharing final : public QObject {
public:
    TargetSessionSharing(AccessStore&, SessionDirectory&, SavedTargets&, QObject* parent);
    void created(const QString&, quint64, const TargetConnectionRequest&,
        const TargetConnectionFacts&, const NewSessionSharing&);
    void clear();
private:
    struct Pending {
        QString client, target, configVersion, sessionId, fingerprint;
        quint64 identityVersion, generation;
        NewSessionSharing sharing;
        QPointer<TerminalSession> session;
        QPointer<ITransport> transport;
        QDeadlineTimer deadline{120000};
    };
    void schedule();
    void process();
    AccessStore& _access;
    SessionDirectory& _sessions;
    SavedTargets& _targets;
    QList<std::shared_ptr<const Pending>> _pending;
    QTimer _timer;
    bool _scheduled{false}, _processing{false};
};
}
