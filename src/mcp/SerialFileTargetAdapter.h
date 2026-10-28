#pragma once
#include "McpAccess.h"
#include "application/SerialFileTargets.h"

namespace NovaTerm::Mcp {
// GUI-thread composition only; reads existing connection facts without opening a port.
class SerialFileTargetAdapter final : public QObject {
    Q_OBJECT
public:
    SerialFileTargetAdapter(AccessStore&, SessionDirectory&, QObject* parent);
    SerialFileTargets& targets() { return _targets; }
    std::optional<SerialFileBinding> liveBinding(const QString& session);
    std::optional<SerialFileTarget> share(const QString& client, quint64 identityVersion,
        const SerialFileBinding& expected, const QString& title);
    Result<QList<SerialFileTarget>> list(const QString& client, quint64 identityVersion);
signals:
    void changed();
private:
    void pruneDisconnected();
    AccessStore& _access;
    SessionDirectory& _sessions;
    SerialFileTargets _targets;
    bool _pruning{false};
};
}
