#include "FileResources.h"
#include "mcp/contracts/Budget.h"
#include <QDir>
#include <QUuid>

namespace NovaTerm::Mcp {
namespace {
QString opaque() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validText(const QString& value, int limit)
{
    if (value.isEmpty() || value.toUtf8().size() > limit || QString::fromUtf8(value.toUtf8()) != value) return false;
    for (const auto ch : value) if (ch.unicode() < 32 || ch.unicode() == 127) return false;
    return true;
}
QString rootPath(const QString& input, bool remote)
{
    // Qt may normalize an extended Windows device prefix away; reject it before normalization.
    if (!validText(input, Budget::MaxFileRootPathBytes) || input.startsWith("//") || input.startsWith("\\\\")) return {};
    const auto path = remote ? input : QDir::fromNativeSeparators(input);
    if (!validText(path, Budget::MaxFileRootPathBytes) || path.contains('\\') || path.startsWith("//")
        || (remote ? !path.startsWith('/') : !QDir::isAbsolutePath(path))) return {};
#ifdef Q_OS_WIN
    if (!remote && (path.size() < 3 || !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
        || path[1] != ':' || path[2] != '/' || path.mid(2).contains(':'))) return {};
#endif
    for (const auto& part : path.split('/', Qt::SkipEmptyParts))
        if (part == "." || part == "..") return {};
    const auto clean = QDir::cleanPath(path);
    if (clean == "/" || (clean.size() == 3 && clean[1] == ':' && clean[2] == '/')) return {};
    return clean;
}
bool samePath(const QString& a, const QString& b, bool remote)
{
#ifdef Q_OS_WIN
    if (!remote) return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    Q_UNUSED(remote);
#endif
    return a == b;
}
}
FileResources::FileResources(Identity identity, QObject* parent) : QObject(parent), _identity(std::move(identity)) {}
bool FileResources::validIdentity(const QString& client, quint64 version) const
{
    return !client.isEmpty() && version && _identity && _identity(client) == version;
}
bool FileResources::sameBinding(const SessionBinding& a, const SessionBinding& b)
{
    return a.session == b.session && a.attachment == b.attachment && a.epoch == b.epoch
        && a.generation == b.generation && a.targetFingerprint == b.targetFingerprint;
}
std::optional<FileRoot> FileResources::shareLocal(const QString& client, quint64 identity,
    const QString& input, const QString& title, FileRootAccess access)
{
    const auto path = rootPath(input, false);
    if (!validIdentity(client, identity) || path.isEmpty() || !validText(title, Budget::MaxFileRootTitleBytes)
        || (!access.read && !access.write)) return std::nullopt;
    int count = 0;
    for (auto it = _local.begin(); it != _local.end(); ++it) {
        if (it->client != client) continue;
        ++count;
        if (it->identityVersion != identity || !samePath(it->root.path, path, false)) continue;
        const FileRoot root{it->root.id, title, opaque(), path, access};
        it->root = root;
        emit changed(); return root;
    }
    if (count >= Budget::MaxFileLocalRootsPerClient || _local.size() >= Budget::MaxFileLocalRoots) return std::nullopt;
    const FileRoot root{opaque(), title, opaque(), path, access};
    _local.insert(root.id, {client, identity, root, {}});
    emit changed(); return root;
}
std::optional<FileRoot> FileResources::shareRemote(const QString& client, quint64 identity, const SessionBinding& binding,
    const QString& input, const QString& title, FileRootAccess access)
{
    const auto path = rootPath(input, true);
    if (!validIdentity(client, identity) || binding.session.isEmpty() || binding.epoch.isEmpty()
        || binding.attachment.isEmpty() || !binding.generation.value() || !binding.targetFingerprint
        || binding.targetFingerprint->isEmpty() || path.isEmpty()
        || !validText(title, Budget::MaxFileRootTitleBytes) || (!access.read && !access.write)) return std::nullopt;
    int count = 0;
    for (auto it = _remote.begin(); it != _remote.end(); ++it) {
        if (it->client != client) continue;
        ++count;
        if (it->identityVersion != identity || !sameBinding(it->binding, binding) || !samePath(it->root.path, path, true)) continue;
        const FileRoot root{it->root.id, title, opaque(), path, access};
        it->root = root;
        emit changed(); return root;
    }
    if (count >= Budget::MaxFileRemoteRootsPerClient || _remote.size() >= Budget::MaxFileRemoteRoots) return std::nullopt;
    const FileRoot root{opaque(), title, opaque(), path, access};
    _remote.insert(root.id, {client, identity, root, binding});
    emit changed(); return root;
}
QList<FileRoot> FileResources::local(const QString& client, quint64 identity) const
{
    QList<FileRoot> rows;
    if (validIdentity(client, identity)) for (const auto& grant : _local)
        if (grant.client == client && grant.identityVersion == identity) rows.append(grant.root);
    return rows;
}
QList<FileRoot> FileResources::remote(const QString& client, quint64 identity, const SessionBinding& binding) const
{
    QList<FileRoot> rows;
    if (validIdentity(client, identity)) for (const auto& grant : _remote)
        if (grant.client == client && grant.identityVersion == identity && sameBinding(grant.binding, binding)) rows.append(grant.root);
    return rows;
}
bool FileResources::remove(const QString& client, quint64 identity, const QString& id, const QString& revision)
{
    if (!validIdentity(client, identity)) return false;
    for (auto* grants : {&_local, &_remote}) {
        const auto it = grants->find(id);
        if (it == grants->end()) continue;
        if (it->client != client || it->identityVersion != identity || it->root.revision != revision) return false;
        grants->erase(it); emit changed(); return true;
    }
    return false;
}
QList<RemoteFileRoot> FileResources::remote(const QString& client, quint64 identity) const
{
    QList<RemoteFileRoot> rows;
    if (validIdentity(client, identity)) for (const auto& grant : _remote)
        if (grant.client == client && grant.identityVersion == identity) rows.append({grant.root, grant.binding});
    return rows;
}
void FileResources::revoke(const QString& client)
{
    bool removed = false;
    for (auto* grants : {&_local, &_remote})
        for (auto it = grants->begin(); it != grants->end();)
            if (client.isEmpty() || it->client == client) { it = grants->erase(it); removed = true; } else ++it;
    if (removed) emit changed();
}
void FileResources::invalidateSession(const QString& session)
{
    bool removed = false;
    for (auto it = _remote.begin(); it != _remote.end();)
        if (it->binding.session.str() == session) { it = _remote.erase(it); removed = true; } else ++it;
    if (removed) emit changed();
}
QStringList FileResources::remoteSessions() const
{
    QStringList sessions;
    for (const auto& grant : _remote)
        if (!sessions.contains(grant.binding.session.str())) sessions.append(grant.binding.session.str());
    return sessions;
}
}
