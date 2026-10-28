#pragma once
#include "mcp/contracts/SessionBinding.h"
#include <QMap>
#include <QObject>
#include <functional>
#include <optional>

namespace NovaTerm::Mcp {
struct SerialFileBinding { SessionBinding connection; quint64 ioGeneration{0}; QString configDigest; };
struct SerialFileTarget { QString id, title, revision; SerialFileBinding binding; };
class SerialFileTargets final : public QObject {
    Q_OBJECT
public:
    using Identity = std::function<quint64(const QString&)>;
    explicit SerialFileTargets(Identity, QObject* parent = nullptr);
    std::optional<SerialFileTarget> share(const QString& client, quint64 identityVersion,
        const SerialFileBinding&, const QString& title);
    QList<SerialFileTarget> list(const QString& client, quint64 identityVersion) const;
    bool remove(const QString& client, quint64 identityVersion, const QString& id, const QString& revision);
    void revoke(const QString& client = {});
    void invalidateSession(const QString& session);
    void invalidateBinding(const SerialFileBinding&);
    QList<SerialFileBinding> bindings() const;
    static bool sameBinding(const SerialFileBinding&, const SerialFileBinding&);
signals:
    void changed();
private:
    struct Grant { QString client; quint64 identityVersion; SerialFileTarget target; };
    bool validIdentity(const QString&, quint64) const;
    Identity _identity;
    QMap<QString, Grant> _grants;
};
}
