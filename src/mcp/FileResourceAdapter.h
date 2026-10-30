#pragma once
#include "McpAccess.h"
#include "application/FileResources.h"
#include "contracts/FileResourceRequest.h"
#include "session/SftpSession.h"
#include <functional>

namespace NovaTerm::Mcp {
struct FileResourceDirectory { QList<FileRoot> local; QList<RemoteFileRoot> remote; };
// GUI-thread authorization/discovery never opens files or connections. Explicit
// human directory browsing may use the injected existing SFTP owner; MCP listing
// does not call that browsing port.
class FileResourceAdapter final : public QObject {
    Q_OBJECT
public:
    FileResourceAdapter(AccessStore&, SessionDirectory&, QObject* parent);
    FileResources& resources() { return _resources; }
    std::optional<FileRoot> shareRemote(const QString& client, quint64 identityVersion,
        const SessionBinding& expected, const QString& path, const QString& title, FileRootAccess);
    Result<FileResourceDirectory> list(const QString& client, quint64 identityVersion, const ListFileResourcesRequest&);
    using DirectorySession = std::function<SftpSession*(TerminalSession&)>;
    using DirectoryCompletion = std::function<void(SftpDirectoryResult)>;
    void setDirectorySessionProvider(DirectorySession provider) { _directorySession = std::move(provider); }
    bool matchesRemoteBinding(QString client, quint64 identity, SessionBinding expected);
    std::optional<SessionBinding> liveBinding(const QString& session);
    void listRemoteDirectory(QString client, quint64 identity, SessionBinding expected,
        QString path, QObject* context, DirectoryCompletion);
signals:
    void changed();
private:
    void pruneDisconnected();
    AccessStore& _access;
    SessionDirectory& _sessions;
    FileResources _resources;
    bool _pruning{false};
    DirectorySession _directorySession;
    int _pendingDirectoryLists{0};
};
}
