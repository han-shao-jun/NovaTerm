/** @file McpAccess.cpp
 *  @brief 仅保存随机接入令牌的摘要，原令牌由平台凭据库保管。
 */
#include "McpAccess.h"
#include "McpProtocol.h"
#include "session/SessionCommandFacade.h"
#include "session/TerminalContextProvider.h"
#include "core/terminal/TerminalCore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>

namespace NovaTerm::Mcp {
QString AccessStore::reference(const QString& id) { return QStringLiteral("mcp-client-") + id; }
CredentialStore* AccessStore::credentials() const
{
    // 仅创建/导出/轮换客户端令牌的用户操作触碰密钥库，协议认证只核对摘要。
    if (!_credentials) _credentials = createCredentialStore();
    return _credentials.get();
}
AccessStore::AccessStore(QString directory, std::unique_ptr<CredentialStore> credentials, QObject* parent)
    : QObject(parent), _directory(std::move(directory)), _credentials(std::move(credentials))
{
    reload();
    if (QFileInfo::exists(_directory)) _watcher.addPath(_directory);
    connect(&_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { reload(); });
}

void AccessStore::reload()
{
    const auto stored = readJson(QDir(_directory).filePath("access.json"));
    if (!stored) {
        if (_enabled || !_clients.isEmpty()) {
            _enabled = false;
            _clients.clear();
            _grants.clear();
            _storeRevision = 0;
            emit revoked({}, {});
            emit changed();
        }
        return;
    }
    const auto revision = stored->value("revision").toString().toULongLong();
    if (revision != 0 && revision == _storeRevision) return;
    QHash<QString, Client> next;
    // 易失接入仅属于本进程，不写入共享文件，也不被其他实例的保存清除。
    for (const auto& client : _clients) if (!client.persistent) next.insert(client.id, client);
    for (const auto& value : stored->value("clients").toArray()) {
        if (next.size() >= 16)
            break;
        const auto object = value.toObject();
        const auto id = object.value("id").toString();
        const auto digest = QByteArray::fromHex(object.value("digest").toString().toLatin1());
        // 内存后端的旧令牌重启后无效，不复活其接入配置。
        if (QUuid(id).isNull() || digest.size() != 32 || !object.value("persistent").toBool())
            continue;
        const auto old = _clients.value(id);
        const auto version = old.digest == digest ? old.version : old.version + 1;
        next.insert(id, Client{id, object.value("label").toString().left(80), digest, true, version});
    }
    QStringList invalidated;
    for (const auto& client : _clients) {
        if (!next.contains(client.id) || next.value(client.id).digest != client.digest)
            invalidated.append(client.id);
    }
    _clients = std::move(next);
    _storeRevision = revision;
    _enabled = stored->value("enabled").toBool();
    for (const auto& id : invalidated) {
        if (!_clients.contains(id)) _grants.remove(id);
        emit revoked(id, {});
    }
    if (!_enabled) { _grants.clear(); emit revoked({}, {}); }
    emit changed();
}

bool AccessStore::save()
{
    if (!secureDirectory(_directory)) return false;
    QLockFile lock(QDir(_directory).filePath("access.lock"));
    if (!lock.tryLock(0)) return false;
    const auto path = QDir(_directory).filePath("access.json");
    const auto current = readJson(path);
    if ((!current && QFileInfo::exists(path)) || (current
        && current->value("revision").toString().toULongLong() != _storeRevision))
        return false;
    QJsonArray clients;
    for (const auto& client : _clients) {
        if (!client.persistent) continue;
        clients.append(QJsonObject{{"id", client.id}, {"label", client.label},
            {"digest", QString::fromLatin1(client.digest.toHex())}, {"persistent", client.persistent}});
    }
    const bool ok = writePrivateJson(path, {{"version", 1}, {"revision", QString::number(_storeRevision + 1)},
        {"enabled", _enabled}, {"clients", clients}});
    if (ok) {
        ++_storeRevision;
        if (_watcher.directories().isEmpty()) _watcher.addPath(_directory);
    }
    return ok;
}

bool AccessStore::setEnabled(bool enabled)
{
    if (!enabled) {
        // 停用先在内存生效；磁盘写入失败也不能让用户无法撤销正在使用的权限。
        _enabled = false;
        _grants.clear();
        for (auto& client : _clients) ++client.version;
        const bool saved = save();
        emit revoked({}, {});
        emit changed();
        return saved;
    }
    const bool previous = _enabled;
    _enabled = enabled;
    if (!save()) { _enabled = previous; return false; }
    if (!enabled)
        emit revoked({}, {});
    emit changed();
    return true;
}

QList<AccessStore::Client> AccessStore::clients() const
{
    auto result = _clients.values();
    std::sort(result.begin(), result.end(), [](const Client& a, const Client& b) { return a.label < b.label; });
    return result;
}

QString AccessStore::addClient(const QString& label)
{
    if (_clients.size() >= 8 || label.trimmed().isEmpty() || !credentials())
        return {};
    const QString id = newId();
    const auto token = randomBytes().toHex();
    if (!_credentials->put(reference(id), token))
        return {};
    _clients.insert(id, Client{id, label.trimmed().left(80),
        QCryptographicHash::hash(token, QCryptographicHash::Sha256), _credentials->isPersistent(), 1});
    if (!save()) {
        _clients.remove(id);
        _credentials->remove(reference(id));
        return {};
    }
    emit changed();
    return id;
}

bool AccessStore::removeClient(const QString& id)
{
    if (!_clients.contains(id) || !credentials())
        return false;
    const auto old = _clients.take(id);
    if (!save()) { _clients.insert(id, old); return false; }
    _grants.remove(id);
    _credentials->remove(reference(id));
    emit revoked(id, {});
    emit changed();
    return true;
}

bool AccessStore::rotateToken(const QString& id)
{
    auto it = _clients.find(id);
    if (it == _clients.end() || !credentials())
        return false;
    const auto old = *it;
    const auto oldToken = _credentials->get(reference(id));
    const auto token = randomBytes().toHex();
    if (!_credentials->put(reference(id), token))
        return false;
    it->digest = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
    it->persistent = _credentials->isPersistent();
    ++it->version;
    if (!save()) {
        *it = old;
        if (oldToken) _credentials->put(reference(id), *oldToken);
        return false;
    }
    emit revoked(id, {});
    emit changed();
    return true;
}

std::optional<QByteArray> AccessStore::exportToken(const QString& id) const
{
    if (!_clients.contains(id) || !credentials())
        return std::nullopt;
    return _credentials->get(reference(id));
}

QString AccessStore::authenticate(const QByteArray& token) const
{
    if (!_enabled || token.size() != 64)
        return {};
    const auto digest = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
    QString result;
    for (const auto& client : _clients) {
        if (constantTimeEqual(digest, client.digest))
            result = client.id;
    }
    return result;
}

quint64 AccessStore::version(const QString& clientId) const
{
    return _clients.value(clientId).version;
}

bool AccessStore::setGrant(const QString& clientId, const SessionDirectory::Entry& entry,
                           bool read, QSet<QString> commands,
                           bool interactiveCommand, bool confirmedCommand,
                           bool scriptTask)
{
    if (!_clients.contains(clientId) || !entry.session || (read && !_enabled))
        return false;
    if (!read) {
        _grants[clientId].remove(entry.id);
    } else {
        const auto* facade = entry.session->commandFacade();
        if (entry.state != SessionState::Running || !facade
            || !facade->isAvailable() || entry.targetFingerprint.isEmpty()) {
            commands.clear();
        }
        _grants[clientId].insert(entry.id,
            Grant{entry.attachmentId, entry.epoch,
                  std::move(commands), interactiveCommand,
                  confirmedCommand, scriptTask});
        // 授予读取共享后提前排一次发布，**缩短**首批读取拿不到可用快照的窗口，
        // 但不保证首个 read_context 可用：发布是 fire-and-forget，真正的构造在
        // 之后的 Parser 线程上，与紧随其后的首个读之间仍有竞态（P8 §14.2 记录的
        // 剩余 Busy 集中在这一窗口）。因此只在会话确实在运行、且本次是首次授予
        // 读取时触发，避免只翻转执行类授权的改动白白占用发布请求与计数器。
        const bool wasShared = canRead(clientId, entry);
        if (!wasShared && entry.state == SessionState::Running
            && NovaTerm::publishedContextSnapshotEnabled() && entry.session->core()) {
            static_cast<void>(entry.session->core()->requestPublishedTerminalState());
        }
    }
    ++_clients[clientId].version;
    emit revoked(clientId, entry.id);
    emit changed();
    return true;
}

bool AccessStore::canRead(const QString& clientId, const SessionDirectory::Entry& entry) const
{
    const auto grants = _grants.constFind(clientId);
    if (!_enabled || !_clients.contains(clientId) || grants == _grants.cend())
        return false;
    const auto grant = grants->constFind(entry.id);
    return grant != grants->cend() && grant->attachment == entry.attachmentId;
}

QSet<QString> AccessStore::commands(const QString& clientId, const SessionDirectory::Entry& entry) const
{
    if (!canRead(clientId, entry))
        return {};
    const auto grant = _grants.value(clientId).value(entry.id);
    return grant.epoch == entry.epoch ? grant.commands : QSet<QString>{};
}

bool AccessStore::canRunCommand(
    const QString& clientId, const SessionDirectory::Entry& entry) const
{
    if (!canRead(clientId, entry))
        return false;
    const auto grant = _grants.value(clientId).value(entry.id);
    return grant.epoch == entry.epoch && grant.interactiveCommand;
}

bool AccessStore::canRunConfirmedCommand(
    const QString& clientId, const SessionDirectory::Entry& entry) const
{
    if (!canRead(clientId, entry))
        return false;
    const auto grant = _grants.value(clientId).value(entry.id);
    return grant.epoch == entry.epoch && grant.confirmedCommand;
}

bool AccessStore::canRunScriptTask(
    const QString& clientId, const SessionDirectory::Entry& entry) const
{
    if (!canRead(clientId, entry))
        return false;
    const auto grant = _grants.value(clientId).value(entry.id);
    return grant.epoch == entry.epoch && grant.scriptTask;
}
}
