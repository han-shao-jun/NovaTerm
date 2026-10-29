#include "FileResourceAdapter.h"
#include "transport/SshTransport.h"
#include <QScopedValueRollback>
#include <QTimer>

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

bool FileResourceAdapter::matchesRemoteBinding(QString client, quint64 identity, SessionBinding expected)
{
    if (!_access.enabled() || !identity || _access.accessId() != client || _access.identityVersion(client) != identity) return false;
    const auto binding = liveBinding(expected.session.str());
    return binding && FileResources::sameBinding(expected, *binding) && _access.enabled()
        && _access.accessId() == client && _access.identityVersion(client) == identity && identity;
}

void FileResourceAdapter::listRemoteDirectory(QString client, quint64 identity,
    SessionBinding expected, QString path, QObject* context, DirectoryCompletion completion)
{
    if (!context || !completion) return;
    const auto failure = [](const QString& message) { return SftpDirectoryResult{{}, {}, message, false}; };
    if (!matchesRemoteBinding(client, identity, expected)) {
        completion(failure(tr("SSH连接或MCP身份已变化，请重新选择。"))); return;
    }
    if (!_directorySession || _pendingDirectoryLists >= 8) {
        completion(failure(tr("SFTP目录浏览不可用或请求过多，请稍后重试。"))); return;
    }
    const auto entry = _sessions.find(expected.session.str());
    if (!entry || !entry->session) { completion(failure(tr("SSH会话已关闭。"))); return; }
    const QPointer<SftpSession> backend = _directorySession(*entry->session);
    if (!backend || !matchesRemoteBinding(client, identity, expected)) {
        completion(failure(tr("当前SSH的SFTP连接未就绪，请稍后重试。"))); return;
    }
    auto* job = new QObject(this);
    ++_pendingDirectoryLists;
    struct State { bool completed{false}; quint64 request{0}; QMetaObject::Connection teardown; };
    const auto state = std::make_shared<State>();
    const auto deliver = std::make_shared<DirectoryCompletion>(std::move(completion));
    const QPointer<QObject> recipient(context);
    const QPointer<FileResourceAdapter> self(this);
    const auto current = [self, client, identity, expected] {
        return self && self->matchesRemoteBinding(client, identity, expected);
    };
    const auto finish = [this, job, recipient, backend, state, deliver](SftpDirectoryResult result) {
        if (state->completed) return;
        state->completed = true;
        QObject::disconnect(state->teardown);
        if (backend && state->request) backend->cancelDirectoryRequest(state->request);
        --_pendingDirectoryLists;
        job->deleteLater();
        if (recipient) (*deliver)(std::move(result));
    };
    // Teardown delivery is queued: the Service/AccessStore may be midway through
    // destruction when QObject::destroyed fires. Recipient guards see the fully
    // destroyed owner before any UI callback revalidates its binding.
    state->teardown = connect(this, &QObject::destroyed, context, [state, deliver, recipient, failure] {
        if (state->completed) return;
        state->completed = true;
        if (recipient) (*deliver)(failure(tr("目录浏览服务已关闭。")));
    }, Qt::QueuedConnection);
    connect(context, &QObject::destroyed, job, [finish, failure] { finish(failure(QStringLiteral("CANCELLED"))); });
    connect(job, &QObject::destroyed, backend, [backend, state] {
        if (backend && state->request) backend->cancelDirectoryRequest(state->request);
    });
    connect(backend, &SftpSession::directoryRequestFinished, job,
        [state, current, finish, failure](quint64 request, const SftpDirectoryResult& result) {
            if (state->completed || request != state->request || !request) return;
            finish(current() ? result : failure(tr("SSH连接或MCP身份已变化，请重新选择。")));
        });
    connect(backend, &QObject::destroyed, job, [finish, failure] { finish(failure(tr("SFTP连接已销毁。"))); });
    connect(backend, &SftpSession::disconnected, job, [finish, failure] { finish(failure(tr("SFTP连接已断开。"))); });
    connect(backend, &SftpSession::errorOccurred, job, [state, finish, failure](const QString& error) {
        if (!state->request) finish(failure(error));
    });
    const auto revalidate = [current, finish, failure] {
        if (!current()) finish(failure(tr("SSH连接或MCP身份已变化，请重新选择。")));
    };
    connect(&_access, &AccessStore::changed, job, revalidate);
    connect(&_sessions, &SessionDirectory::changed, job, revalidate);
    const auto start = [state, backend, current, path, finish, failure] {
        if (state->completed || state->request) return;
        if (!current() || !backend) { finish(failure(tr("SSH连接或MCP身份已变化，请重新选择。"))); return; }
        state->request = backend->requestDirectory(path);
        if (!state->request) finish(failure(tr("目录路径无效或SFTP队列繁忙，请稍后重试。")));
    };
    connect(backend, &SftpSession::connected, job, [start](const QString&) { start(); });
    QTimer::singleShot(15000, job, [finish, failure] { finish(failure(tr("读取远端目录超时，请重试。"))); });
    if (backend->isConnected()) start();
}
}
