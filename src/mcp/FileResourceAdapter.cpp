#include "FileResourceAdapter.h"
#include "transport/SshTransport.h"
#include <QScopedValueRollback>

namespace NovaTerm::Mcp {
FileResourceAdapter::FileResourceAdapter(AccessStore& access, SessionDirectory& sessions, QObject* parent)
    : QObject(parent), _access(access), _sessions(sessions),
      _resources([this](const QString& client) { return _access.enabled() ? _access.identityVersion(client) : 0; })
{
    connect(&_resources, &FileResources::changed, this, &FileResourceAdapter::changed);
    connect(&_access, &AccessStore::revoked, this, [this](const QString& client, const QString& session) {
        if (session.isEmpty()) _resources.revoke(client);
    });
    connect(&_sessions, &SessionDirectory::invalidated, this, [this](const QString& session) {
        _resources.invalidateSession(session);
    });
    connect(&_sessions, &SessionDirectory::changed, this, [this] { pruneDisconnected(); });
}
void FileResourceAdapter::pruneDisconnected()
{
    if (_pruning) return;
    const QScopedValueRollback<bool> pruning(_pruning, true);
    const auto sessions = _resources.remoteSessions();
    for (const auto& session : sessions)
        if (!liveBinding(session)) _resources.invalidateSession(session);
}
std::optional<SessionBinding> FileResourceAdapter::liveBinding(const QString& id)
{
    const auto entry = _sessions.find(id);
    if (!entry || !entry->session || entry->kind != TransportKind::Ssh
        || entry->state != SessionState::Running || entry->session->state() != SessionState::Running
        || entry->session->transport() != entry->transport || entry->session->statistics().generation != entry->generation
        || entry->targetFingerprint.isEmpty()) return std::nullopt;
    const auto* ssh = qobject_cast<SshTransport*>(entry->transport.data());
    if (!ssh || !ssh->isConnected() || ssh->hasPendingDisconnect() || ssh->serverHostKeyFingerprint().isEmpty()
        || ssh->property("novatermHostKeyDecisionPending").toBool()) return std::nullopt;
    return SessionBinding{SessionId{entry->id}, AttachmentId{entry->attachmentId}, Epoch{entry->epoch},
        Generation{entry->generation}, entry->targetFingerprint};
}
std::optional<FileRoot> FileResourceAdapter::shareRemote(const QString& client, quint64 identity,
    const SessionBinding& expected, const QString& path, const QString& title, FileRootAccess access)
{
    const auto binding = liveBinding(expected.session.str());
    if (!binding || !FileResources::sameBinding(expected, *binding)) return std::nullopt;
    return _resources.shareRemote(client, identity, *binding, path, title, access);
}
Result<FileResourceDirectory> FileResourceAdapter::list(const QString& client, quint64 identity,
    const ListFileResourcesRequest& request)
{
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return Error{ErrorCode::PermissionDenied};
    QList<RemoteFileRoot> remote;
    const auto candidates = _resources.remote(client, identity);
    for (const auto& candidate : candidates) {
        if (request.reference && candidate.binding.session != request.reference->session) continue;
        const auto binding = liveBinding(candidate.binding.session.str());
        if (!binding || !FileResources::sameBinding(candidate.binding, *binding)) continue;
        // find() may have emitted invalidation callbacks; re-read the live grant before disclosure.
        const auto current = _resources.remote(client, identity, *binding);
        for (const auto& root : current) {
            if (root.id != candidate.root.id || root.revision != candidate.root.revision) continue;
            if (request.reference && request.reference->epoch != binding->epoch) return Error{ErrorCode::StaleSessionEpoch};
            remote.append({root, *binding});
        }
    }
    if (request.reference && remote.isEmpty()) return Error{ErrorCode::PermissionDenied};
    return FileResourceDirectory{_resources.local(client, identity), remote};
}
}
