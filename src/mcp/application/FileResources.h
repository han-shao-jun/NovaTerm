#pragma once
#include "mcp/contracts/SessionBinding.h"
#include <QMap>
#include <QObject>
#include <QStringList>
#include <functional>
#include <optional>

namespace NovaTerm::Mcp {
struct FileRootAccess { bool read{false}, write{false}; };
// Trusted local views may inspect path; wire projection must omit every local path.
struct FileRoot { QString id, title, revision, path; FileRootAccess access; };
struct RemoteFileRoot { FileRoot root; SessionBinding binding; };
class FileResources final : public QObject {
    Q_OBJECT
public:
    using Identity = std::function<quint64(const QString&)>;
    explicit FileResources(Identity, QObject* parent = nullptr);
    std::optional<FileRoot> shareLocal(const QString& client, quint64 identityVersion,
        const QString& path, const QString& title, FileRootAccess);
    std::optional<FileRoot> shareRemote(const QString& client, quint64 identityVersion,
        const SessionBinding&, const QString& path, const QString& title, FileRootAccess);
    QList<FileRoot> local(const QString& client, quint64 identityVersion) const;
    QList<FileRoot> remote(const QString& client, quint64 identityVersion, const SessionBinding&) const;
    QList<RemoteFileRoot> remote(const QString& client, quint64 identityVersion) const;
    bool remove(const QString& client, quint64 identityVersion, const QString& id, const QString& revision);
    void revoke(const QString& client = {});
    void invalidateSession(const QString& session);
    QStringList remoteSessions() const;
    static bool sameBinding(const SessionBinding&, const SessionBinding&);
signals:
    void changed();
private:
    struct Grant { QString client; quint64 identityVersion; FileRoot root; SessionBinding binding; };
    bool validIdentity(const QString&, quint64) const;
    Identity _identity;
    QMap<QString, Grant> _local, _remote;
};
}
