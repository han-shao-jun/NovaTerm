/** @file McpAccess.cpp
 *  @brief 仅保存随机接入令牌的摘要，原令牌由平台凭据库保管。
 */
#include "McpAccess.h"
#include "McpProtocol.h"
#include "session/SessionCommandFacade.h"
#include "session/TerminalContextProvider.h"
#include "session/LocalSessionCommandExecutor.h"
#include "session/SshSessionCommandExecutor.h"
#include "transport/SshTransport.h"
#include "core/terminal/TerminalCore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSysInfo>

namespace NovaTerm::Mcp {
QString AccessStore::reference(const QString& id) { return QStringLiteral("mcp-client-") + id; }
CredentialStore* AccessStore::credentials() const
{
    // 仅创建/导出/轮换客户端令牌的用户操作触碰密钥库，协议认证只核对摘要。
    if (!_credentials) _credentials = createCredentialStore();
    return _credentials.get();
}
AccessStore::AccessStore(QString directory, std::unique_ptr<CredentialStore> credentials, QObject* parent,
    std::function<std::unique_ptr<CredentialStore>()> elevationFactory)
    : QObject(parent), _directory(std::move(directory)), _credentials(std::move(credentials)),
      _elevationFactory(std::move(elevationFactory))
{
    if (!_elevationFactory) {
        if (dynamic_cast<MemoryCredentialStore*>(_credentials.get()))
            _elevationFactory = [] { return std::make_unique<MemoryCredentialStore>(); };
        else _elevationFactory = createWorkerCredentialStore;
    }
    reload();
    if (QFileInfo::exists(_directory)) _watcher.addPath(_directory);
    connect(&_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { reload(); });
}

AccessStore::~AccessStore()
{
    if (_elevationThread) {
        _elevationThread->requestInterruption();
        _elevationThread->quit();
        _elevationThread->wait();
    }
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
    const auto elevationRevision = stored->value("elevationRevision").toString().toULongLong();
    if (elevationRevision != _elevationRevision) {
        for (auto client = _grants.begin(); client != _grants.end(); ++client)
            for (auto grant = client->begin(); grant != client->end(); ++grant)
                grant->useElevation = false;
        for (auto client = _clients.begin(); client != _clients.end(); ++client) ++client->version;
        emit revoked({}, {});
    }
    _elevationRevision = elevationRevision;
    // 只加载引用名，绝不为状态/协议刷新解锁密钥环或读取密码。
    if (_elevationPersistent.value_or(!_credentials || _credentials->isPersistent())) {
        _elevationReferences.clear();
        for (const auto& value : stored->value("elevationReferences").toArray()) {
            const auto ref = value.toString();
            if (_elevationReferences.size() >= 256) break;
            if (ref.startsWith("mcp-elevation-") && ref.size() == 78)
                _elevationReferences.insert(ref);
        }
    }
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
    // 保存失败有四种互不相同的成因：状态目录没通过 0700 与属主校验、共享文件被
    // 另一个实例持锁占用、文件已被并发改写、写盘本身失败。它们的处置完全不同，
    // 因此逐个记下原因交给界面，而不是让上层只看到一个 false。
    _lastSaveFailure = SaveFailure::None;
    if (!secureDirectory(_directory)) {
        _lastSaveFailure = SaveFailure::Directory;
        return false;
    }
    QLockFile lock(QDir(_directory).filePath("access.lock"));
    if (!lock.tryLock(0)) {
        _lastSaveFailure = SaveFailure::Locked;
        return false;
    }
    const auto path = QDir(_directory).filePath("access.json");
    const auto current = readJson(path);
    if ((!current && QFileInfo::exists(path)) || (current
        && current->value("revision").toString().toULongLong() != _storeRevision)) {
        _lastSaveFailure = SaveFailure::Revision;
        return false;
    }
    QJsonArray clients;
    for (const auto& client : _clients) {
        if (!client.persistent) continue;
        clients.append(QJsonObject{{"id", client.id}, {"label", client.label},
            {"digest", QString::fromLatin1(client.digest.toHex())}, {"persistent", client.persistent}});
    }
    QJsonArray elevationReferences;
    if (_elevationPersistent.value_or(!_credentials || _credentials->isPersistent()))
        for (const auto& ref : _elevationReferences) elevationReferences.append(ref);
    const bool ok = writePrivateJson(path, {{"version", 1}, {"revision", QString::number(_storeRevision + 1)},
        {"enabled", _enabled}, {"clients", clients}, {"elevationReferences", elevationReferences},
        {"elevationRevision", QString::number(_elevationRevision)}});
    if (!ok) _lastSaveFailure = SaveFailure::Write;
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
        for (auto it = _clients.begin(); it != _clients.end(); ++it)
            ++it->version;
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
    const bool wasShared = canRead(clientId, entry);
    if (!read) {
        _grants[clientId].remove(entry.id);
    } else {
        const auto previous = _grants.value(clientId).value(entry.id);
        const bool preserveTerminal = previous.attachment == entry.attachmentId
            && previous.epoch == entry.epoch && previous.targetFingerprint == entry.targetFingerprint;
        const auto* facade = entry.session->commandFacade();
        if (entry.state != SessionState::Running || !facade
            || !facade->isAvailable() || entry.targetFingerprint.isEmpty()) {
            commands.clear();
        }
        _grants[clientId].insert(entry.id,
            Grant{entry.attachmentId, entry.epoch,
                  std::move(commands), interactiveCommand,
                  confirmedCommand, scriptTask,
                  preserveTerminal && previous.terminalControl,
                  preserveTerminal && previous.useElevation, entry.targetFingerprint});
        // 授予读取共享后提前排一次发布，**缩短**首批读取拿不到可用快照的窗口，
        // 但不保证首个 read_context 可用：发布是 fire-and-forget，真正的构造在
        // 之后的 Parser 线程上，与紧随其后的首个读之间仍有竞态（P8 §14.2 记录的
        // 剩余 Busy 集中在这一窗口）。因此只在会话确实在运行、且本次是首次授予
        // 读取时触发，避免只翻转执行类授权的改动白白占用发布请求与计数器。
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

bool AccessStore::setTerminalGrant(const QString& clientId,
    const SessionDirectory::Entry& entry, bool control, bool elevation)
{
    if (!canRead(clientId, entry) || !entry.session) return false;
    auto& grant = _grants[clientId][entry.id];
    if (control && (entry.state != SessionState::Running || entry.epoch.isEmpty()
        || !entry.transport || !entry.transport->isConnected()
        || entry.session->transport() != entry.transport
        || entry.session->statistics().generation != entry.generation)) return false;
    if (elevation && (!control || _credentialOperationPending
        || elevationReference(entry, "sudo").isEmpty())) return false;
    if (grant.epoch != entry.epoch || grant.targetFingerprint != entry.targetFingerprint) {
        grant.commands.clear();
        grant.interactiveCommand = false;
        grant.confirmedCommand = false;
        grant.scriptTask = false;
    }
    // 更新旧接口权限不能跨 epoch 复活完整输入权限。
    grant.epoch = entry.epoch;
    grant.targetFingerprint = entry.targetFingerprint;
    grant.terminalControl = control;
    grant.useElevation = control && elevation;
    ++_clients[clientId].version;
    emit revoked(clientId, entry.id);
    emit changed();
    return true;
}

bool AccessStore::canControlTerminal(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    if (!canRead(clientId, entry) || !entry.session || entry.state != SessionState::Running
        || entry.session->state() != SessionState::Running || !entry.transport
        || !entry.transport->isConnected() || entry.session->transport() != entry.transport
        || entry.session->statistics().generation != entry.generation) return false;
    const auto grant = _grants.value(clientId).value(entry.id);
    return grant.terminalControl && grant.epoch == entry.epoch
        && grant.targetFingerprint == entry.targetFingerprint;
}

bool AccessStore::canUseElevation(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    return canControlTerminal(clientId, entry)
        && _grants.value(clientId).value(entry.id).useElevation;
}

QString AccessStore::elevationReference(const SessionDirectory::Entry& entry,
    const QString& method) const
{
    if ((method != "sudo" && method != "su") || !entry.session || !entry.transport
        || entry.state != SessionState::Running || entry.session->state() != SessionState::Running
        || entry.session->transport() != entry.transport || !entry.transport->isConnected()
        || entry.session->statistics().generation != entry.generation
        || entry.kind != entry.session->runtimeConfig().transportKind
        || entry.targetFingerprint.isEmpty()) return {};
    QByteArray identity = QSysInfo::machineUniqueId() + '\0' + QDir::homePath().toUtf8() + '\0';
    if (entry.kind == TransportKind::LocalShell) {
        LocalSessionCommandExecutor local(QString{});
        if (local.targetFingerprint() != entry.targetFingerprint) return {};
        identity += "local\0";
    } else if (entry.kind == TransportKind::Ssh) {
        auto* ssh = qobject_cast<SshTransport*>(entry.transport.data());
        if (!ssh || ssh->serverHostKeyFingerprint().isEmpty()) return {};
        SshSessionCommandExecutor executor(ssh);
        if (executor.targetFingerprint() != entry.targetFingerprint) return {};
        const auto& config = ssh->sessionConfig();
        if (config.host.isEmpty() || config.host.contains(QChar::Null)
            || config.username.trimmed().isEmpty() || config.username.contains(QChar::Null)) return {};
        identity += "ssh\0";
        identity += config.host.toUtf8() + '\0' + QByteArray::number(config.port) + '\0'
            + config.username.trimmed().toUtf8() + '\0' + ssh->serverHostKeyFingerprint().toUtf8() + '\0';
    } else return {};
    identity += entry.targetFingerprint.toUtf8() + '\0' + method.toLatin1();
    return QStringLiteral("mcp-elevation-")
        + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

bool AccessStore::hasElevationCredential(const SessionDirectory::Entry& entry,
    const QString& method) const
{
    const auto ref = elevationReference(entry, method);
    return !ref.isEmpty() && _elevationReferences.contains(ref);
}

bool AccessStore::elevationCredentialsPersistent() const
{
    return _elevationPersistent.value_or(_credentials && _credentials->isPersistent());
}

void AccessStore::revokeElevation(const QString& targetFingerprint)
{
    for (auto clients = _grants.begin(); clients != _grants.end(); ++clients) {
        bool invalidated = false;
        for (auto grant = clients->begin(); grant != clients->end(); ++grant) {
            if (grant->targetFingerprint == targetFingerprint && grant->useElevation) {
                grant->useElevation = false;
                invalidated = true;
                emit revoked(clients.key(), grant.key());
            }
        }
        if (invalidated) ++_clients[clients.key()].version;
    }
}

bool AccessStore::setElevationCredential(const SessionDirectory::Entry& entry,
    const QString& method, const QByteArray& secret)
{
    const auto ref = elevationReference(entry, method);
    if (_credentialOperationPending || ref.isEmpty() || secret.isEmpty() || secret.size() > 4096
        || (!_elevationReferences.contains(ref) && _elevationReferences.size() >= 256)
        || !credentials()) return false;
    // This local configuration operation is the only write path; no MCP request calls it.
    if (!_credentials->put(ref, secret)) return false;
    _elevationReferences.insert(ref);
    revokeElevation(entry.targetFingerprint);
    ++_elevationRevision;
    const bool saved = save();
    emit changed();
    return saved;
}

bool AccessStore::configureElevationCredentialAsync(const SessionDirectory::Entry& entry,
    const QString& method, QByteArray secret, bool remove, std::function<void(bool)> finished)
{
    const auto ref = elevationReference(entry, method);
    if (_credentialOperationPending || ref.isEmpty()
        || (remove ? !_elevationReferences.contains(ref) : secret.isEmpty() || secret.size() > 4096)
        || (!remove && !_elevationReferences.contains(ref) && _elevationReferences.size() >= 256)) {
        secret.fill('\0');
        return false;
    }
    if (!_elevationThread) {
        _elevationThread = std::make_unique<QThread>();
        _elevationStorage = std::make_shared<std::unique_ptr<CredentialStore>>();
        _elevationWorker = new QObject;
        _elevationWorker->moveToThread(_elevationThread.get());
        connect(_elevationThread.get(), &QThread::finished, _elevationWorker,
            [holder = _elevationStorage] { holder->reset(); }, Qt::DirectConnection);
        connect(_elevationThread.get(), &QThread::finished, _elevationWorker, &QObject::deleteLater);
        _elevationThread->start();
    }
    _credentialOperationPending = true;
    revokeElevation(entry.targetFingerprint);
    emit changed();
    const auto holder = _elevationStorage;
    const auto factory = _elevationFactory;
    QMetaObject::invokeMethod(_elevationWorker,
        [this, holder, factory, ref, target = entry.targetFingerprint, remove,
         secret = std::move(secret), finished = std::move(finished)]() mutable {
        if (!*holder) *holder = factory();
        const bool persistent = *holder && (*holder)->isPersistent();
        const bool ok = *holder && (remove ? (*holder)->remove(ref) : (*holder)->put(ref, secret));
        secret.fill('\0');
        QMetaObject::invokeMethod(this,
            [this, ref, target, remove, ok, persistent, finished = std::move(finished)] {
            _credentialOperationPending = false;
            _elevationPersistent = persistent;
            bool saved = false;
            if (ok) {
                if (remove) _elevationReferences.remove(ref);
                else _elevationReferences.insert(ref);
                revokeElevation(target);
                ++_elevationRevision;
                saved = save();
            }
            emit changed();
            if (finished) finished(ok && saved);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return true;
}

bool AccessStore::removeElevationCredential(const SessionDirectory::Entry& entry,
    const QString& method)
{
    const auto ref = elevationReference(entry, method);
    if (_credentialOperationPending || ref.isEmpty()
        || !_elevationReferences.contains(ref) || !credentials()) return false;
    // Revoke first, including when the system keyring refuses deletion.
    revokeElevation(entry.targetFingerprint);
    if (!_credentials->remove(ref)) { emit changed(); return false; }
    _elevationReferences.remove(ref);
    ++_elevationRevision;
    const bool saved = save();
    emit changed();
    return saved;
}
}
