#pragma once
#include "McpAccess.h"
#include "application/FileResources.h"
#include "contracts/FileResourceRequest.h"

namespace NovaTerm::Mcp {
struct FileResourceDirectory { QList<FileRoot> local; QList<RemoteFileRoot> remote; };
// GUI-thread composition only. Never opens a file or creates a connection.
class FileResourceAdapter final : public QObject {
    Q_OBJECT
public:
    FileResourceAdapter(AccessStore&, SessionDirectory&, QObject* parent);
    FileResources& resources() { return _resources; }
    std::optional<FileRoot> shareRemote(const QString& client, quint64 identityVersion,
        const SessionBinding& expected, const QString& path, const QString& title, FileRootAccess);
    Result<FileResourceDirectory> list(const QString& client, quint64 identityVersion, const ListFileResourcesRequest&);
signals:
    void changed();
private:
    std::optional<SessionBinding> liveBinding(const QString& session);
    void pruneDisconnected();
    AccessStore& _access;
    SessionDirectory& _sessions;
    FileResources _resources;
    bool _pruning{false};
};
}
