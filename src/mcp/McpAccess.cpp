/** @file McpAccess.cpp
 *  @brief 仅保存随机接入令牌的摘要，原令牌由平台凭据库保管。
 */
#include "McpAccess.h"
#include "McpProtocol.h"
#include "core/ThreadNaming.h"
#include "mcp/application/CapabilityResolver.h"
#include "CommandPolicy.h"
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
#include <mutex>

namespace NovaTerm::Mcp {
struct AccessStore::CredentialMailbox {
    explicit CredentialMailbox(AccessStore* value) : receiver(value) {}
    std::mutex mutex;
    AccessStore* receiver;
};
namespace {
struct CredentialSecret {
    explicit CredentialSecret(QByteArray value) : bytes(std::move(value)) {}
    ~CredentialSecret() { bytes.fill('\0'); }
    QByteArray bytes;
};
}

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
      _authorization(AuthorizationPorts{
          [this] { return _enabled; },
          [this](const ClientId& id) { return _clients.contains(id); },
          [this](const ClientId& id) { return _clients.version(id); },
          [this](const ClientId& id) { return _clients.advance(id); },
          [](const DiagnosticId& id) { return CommandPolicy::find(id.str()).has_value(); },
          [this] { return _credentialOperationPending; }}),
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
    if (_elevationMailbox) {
        const std::lock_guard<std::mutex> lock(_elevationMailbox->mutex);
        _elevationMailbox->receiver = nullptr;
    }
    if (_elevationThread) {
        _elevationThread->requestInterruption();
        _elevationThread->quit();
    }
}

void AccessStore::reload()
{
    const auto stored = readJson(QDir(_directory).filePath("access.json"));
    if (!stored) {
        if (_enabled || _clients.size() > 0) {
            _enabled = false;
            _clients.clear();
            _authorization.clear();
            _storeRevision = 0;
            emit revoked({}, {});
            emit changed();
        }
        return;
    }
    const auto revision = stored->value("revision").toString().toULongLong();
    if (revision != 0 && revision == _storeRevision) return;
    QList<ClientRecord> next;
    // 易失接入仅属于本进程，不写入共享文件，也不被其他实例的保存清除。
    for (const auto& client : _clients.clients())
        if (!client.persistent) next.append(client);
    for (const auto& value : stored->value("clients").toArray()) {
        if (next.size() >= 16)
            break;
        const auto object = value.toObject();
        const auto id = ClientId{object.value("id").toString()};
        const auto digest = QByteArray::fromHex(object.value("digest").toString().toLatin1());
        // 内存后端的旧令牌重启后无效，不复活其接入配置。
        if (id.isEmpty() || QUuid(id.str()).isNull() || digest.size() != 32
            || !object.value("persistent").toBool())
            continue;
        const auto old = _clients.find(id);
        const auto oldVersion = old ? old->version.value() : quint64(1);
        const auto version = old && old->digest == digest ? oldVersion : oldVersion + 1;
        next.append(ClientRecord{id, object.value("label").toString().left(80), digest,
            true, ClientAccessVersion{version}, old && old->digest == digest ? old->identityVersion : quint64(0)});
    }
    QStringList invalidated;
    for (const auto& client : _clients.clients()) {
        const auto found = std::find_if(next.cbegin(), next.cend(),
            [&client](const ClientRecord& value) { return value.id == client.id; });
        if (found == next.cend() || found->digest != client.digest)
            invalidated.append(client.id.str());
    }
    _clients.replaceAll(std::move(next));
    const auto elevationRevision = stored->value("elevationRevision").toString().toULongLong();
    if (elevationRevision != _elevationRevision) {
        static_cast<void>(_authorization.revokeAllElevation());
        _clients.advanceAll();
        _clients.advanceAllIdentities();
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
        if (!_clients.contains(ClientId{id})) _authorization.removeClient(ClientId{id});
        emit revoked(id, {});
    }
    if (!_enabled) { _authorization.clear(); _clients.advanceAllIdentities(); emit revoked({}, {}); }
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
    for (const auto& client : _clients.clients()) {
        if (!client.persistent) continue;
        clients.append(QJsonObject{{"id", client.id.str()}, {"label", client.label},
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
        _authorization.clear();
        _clients.advanceAll();
        _clients.advanceAllIdentities();
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

QList<AccessStore::Client> AccessStore::clients() const { return _clients.clients(); }

QString AccessStore::addClient(const QString& label)
{
    if (_clients.size() >= 8 || label.trimmed().isEmpty() || !credentials())
        return {};
    const QString id = newId();
    const auto token = randomBytes().toHex();
    if (!_credentials->put(reference(id), token))
        return {};
    _clients.insert(ClientRecord{ClientId{id}, label.trimmed().left(80),
        QCryptographicHash::hash(token, QCryptographicHash::Sha256),
        _credentials->isPersistent(), ClientAccessVersion{1}});
    if (!save()) {
        _clients.remove(ClientId{id});
        _credentials->remove(reference(id));
        return {};
    }
    emit changed();
    return id;
}

bool AccessStore::removeClient(const QString& id)
{
    const auto old = _clients.find(ClientId{id});
    if (!old || !credentials())
        return false;
    _clients.remove(ClientId{id});
    if (!save()) { _clients.insert(*old); return false; }
    _authorization.removeClient(ClientId{id});
    _credentials->remove(reference(id));
    emit revoked(id, {});
    emit changed();
    return true;
}

bool AccessStore::rotateToken(const QString& id)
{
    const auto old = _clients.find(ClientId{id});
    if (!old || !credentials())
        return false;
    const auto oldToken = _credentials->get(reference(id));
    const auto token = randomBytes().toHex();
    if (!_credentials->put(reference(id), token))
        return false;
    _clients.updateDigest(ClientId{id},
        QCryptographicHash::hash(token, QCryptographicHash::Sha256),
        _credentials->isPersistent());
    if (!save()) {
        _clients.insert(*old);
        if (oldToken) _credentials->put(reference(id), *oldToken);
        return false;
    }
    emit revoked(id, {});
    emit changed();
    return true;
}

std::optional<QByteArray> AccessStore::exportToken(const QString& id) const
{
    if (!_clients.contains(ClientId{id}) || !credentials())
        return std::nullopt;
    return _credentials->get(reference(id));
}

QString AccessStore::authenticate(const QByteArray& token) const
{
    if (!_enabled || token.size() != 64)
        return {};
    const auto digest = QCryptographicHash::hash(token, QCryptographicHash::Sha256);
    QString result;
    for (const auto& client : _clients.clients()) {
        if (constantTimeEqual(digest, client.digest))
            result = client.id.str();
    }
    return result;
}

quint64 AccessStore::version(const QString& clientId) const
{
    return _clients.version(ClientId{clientId}).value();
}

quint64 AccessStore::identityVersion(const QString& clientId) const
{
    return _clients.identityVersion(ClientId{clientId});
}

std::optional<GrantRow> AccessStore::grantRow(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    if (!_enabled || !_clients.contains(ClientId{clientId}) || entry.id.isEmpty())
        return std::nullopt;
    const auto row = _authorization.row(ClientId{clientId}, SessionId{entry.id});
    if (!row || row->binding.attachment.str() != entry.attachmentId)
        return std::nullopt;
    return row;
}

GrantFacts AccessStore::factsOf(const SessionDirectory::Entry& entry)
{
    GrantFacts facts;
    facts.binding.session = SessionId{entry.id};
    facts.binding.attachment = AttachmentId{entry.attachmentId};
    facts.binding.epoch = Epoch{entry.epoch};
    facts.binding.generation = Generation{entry.generation};
    if (!entry.targetFingerprint.isEmpty())
        facts.binding.targetFingerprint = entry.targetFingerprint;
    facts.sessionRunning = entry.state == SessionState::Running
        && entry.session && entry.session->state() == SessionState::Running;
    facts.transportConnected = entry.transport && entry.transport->isConnected();
    facts.transportMatchesSession = entry.session
        && entry.session->transport() == entry.transport;
    facts.generationMatches = entry.session
        && entry.session->statistics().generation == entry.generation;
    return facts;
}

GrantSnapshot AccessStore::grantSnapshot(const ClientId& client,
    const SessionDirectory::Entry& entry) const
{
    return _authorization.snapshot(client, factsOf(entry));
}

Result<GrantSnapshot, AuthorizationError> AccessStore::changePermission(
    const PermissionChange& change, const SessionDirectory::Entry& entry)
{
    if (!entry.session) {
        return AuthorizationError{AuthorizationRejection::InvalidSelection,
            grantSnapshot(change.client, entry)};
    }
    const bool wasShared = canRead(change.client.str(), entry);
    auto result = _authorization.change(change, factsOf(entry));
    if (isError(result))
        return result;
    emit revoked(change.client.str(), entry.id);
    emit changed();
    // 首次授予读取后提前排一次发布，缩短首个 read_context 的不可用窗口；
    // 只翻转执行类授权时不占用发布请求（与旧 setGrant 的既有行为一致）。
    if (!wasShared && change.kind == PermissionKind::ReadOutput && change.selected
        && entry.state == SessionState::Running
        && NovaTerm::publishedContextSnapshotEnabled() && entry.session->core()) {
        static_cast<void>(entry.session->core()->requestPublishedTerminalState());
    }
    return result;
}

bool AccessStore::canRead(const QString& clientId, const SessionDirectory::Entry& entry) const
{
    const auto row = grantRow(clientId, entry);
    return row && row->selection.read;
}

QSet<QString> AccessStore::commands(const QString& clientId, const SessionDirectory::Entry& entry) const
{
    QSet<QString> result;
    const auto row = grantRow(clientId, entry);
    if (!row || !row->selection.read || row->binding.epoch.str() != entry.epoch)
        return result;
    for (const auto& id : row->selection.diagnostics)
        result.insert(id.str());
    return result;
}

bool AccessStore::canRunCommand(
    const QString& clientId, const SessionDirectory::Entry& entry) const
{
    const auto row = grantRow(clientId, entry);
    return row && row->selection.read && row->binding.epoch.str() == entry.epoch
        && row->selection.riskControlledCommand;
}

bool AccessStore::canRunScriptTask(
    const QString& clientId, const SessionDirectory::Entry& entry) const
{
    const auto row = grantRow(clientId, entry);
    return row && row->selection.read && row->binding.epoch.str() == entry.epoch
        && row->selection.script;
}

bool AccessStore::canControlTerminal(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    if (!entry.session || entry.state != SessionState::Running
        || entry.session->state() != SessionState::Running || !entry.transport
        || !entry.transport->isConnected() || entry.session->transport() != entry.transport
        || entry.session->statistics().generation != entry.generation) return false;
    const auto row = grantRow(clientId, entry);
    return row && row->selection.read && row->binding.epoch.str() == entry.epoch
        && row->binding.targetFingerprint.value_or(QString{}) == entry.targetFingerprint
        && row->selection.fullTerminalInput;
}

bool AccessStore::canUseElevation(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    const auto row = grantRow(clientId, entry);
    return canControlTerminal(clientId, entry) && row && row->selection.elevation;
}

bool AccessStore::canClearTerminal(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    const auto facts = factsOf(entry);
    if (!facts.sessionRunning || !facts.transportConnected
        || !facts.transportMatchesSession || !facts.generationMatches
        || facts.binding.epoch.isEmpty() || facts.binding.attachment.isEmpty())
        return false;
    const auto row = grantRow(clientId, entry);
    return row && row->selection.read && row->selection.clearTerminal
        && row->binding.epoch == facts.binding.epoch
        && row->binding.generation == facts.binding.generation
        && row->binding.targetFingerprint == facts.binding.targetFingerprint;
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

QString AccessStore::elevationGrantDisabledReason(const QString& clientId,
    const SessionDirectory::Entry& entry) const
{
    CapabilityFacts facts;
    facts.sessionRunning = entry.session && entry.state == SessionState::Running
        && entry.session->state() == SessionState::Running;
    facts.transportConnected = entry.transport && entry.transport->isConnected();
    facts.transportMatchesSession = entry.session
        && entry.session->transport() == entry.transport;
    facts.generationMatches = entry.session
        && entry.session->statistics().generation == entry.generation;
    return CapabilityResolver::elevationGrantDisabledReason(
        facts, grantSnapshot(ClientId{clientId}, entry), _credentialOperationPending);
}

bool AccessStore::elevationTargetIdentityVerified(const SessionDirectory::Entry& entry) const
{
    return !elevationReference(entry, QStringLiteral("sudo")).isEmpty();
}

QString AccessStore::elevationExecutionUnavailableReason(const SessionDirectory::Entry& entry) const
{
    // 授权不代表取密/执行许可：所有类型仍须有已验收的可信认证适配器。
    return CapabilityResolver::elevationExecutionUnavailableReason(
        elevationTargetIdentityVerified(entry));
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
    const auto revokedRows = _authorization.revokeElevation(targetFingerprint);
    for (const auto& row : revokedRows) {
        static_cast<void>(_clients.advance(row.first));
        emit revoked(row.first.str(), row.second.str());
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
        _elevationThread = new QThread;
        _elevationMailbox = std::make_shared<CredentialMailbox>(this);
        connect(_elevationThread, &QThread::finished, _elevationThread, &QObject::deleteLater);
        _elevationStorage = std::make_shared<std::unique_ptr<CredentialStore>>();
        _elevationWorker = new QObject;
        _elevationWorker->moveToThread(_elevationThread.data());
        connect(_elevationThread.data(), &QThread::finished, _elevationWorker,
            [holder = _elevationStorage] { holder->reset(); }, Qt::DirectConnection);
        connect(_elevationThread.data(), &QThread::finished, _elevationWorker, &QObject::deleteLater);
        _elevationThread->start();
    }
    _credentialOperationPending = true;
    revokeElevation(entry.targetFingerprint);
    emit changed();
    const auto holder = _elevationStorage;
    const auto factory = _elevationFactory;
    const auto mailbox = _elevationMailbox;
    auto protectedSecret = std::make_shared<CredentialSecret>(std::move(secret));
    QMetaObject::invokeMethod(_elevationWorker,
        [mailbox, holder, factory, ref, target = entry.targetFingerprint, remove,
         protectedSecret = std::move(protectedSecret), finished = std::move(finished)]() mutable {
        NovaTerm::setCurrentThreadName("nvterm-mcp-cred");
        if (!*holder) *holder = factory();
        const bool persistent = *holder && (*holder)->isPersistent();
        const bool ok = *holder && (remove ? (*holder)->remove(ref) : (*holder)->put(ref, protectedSecret->bytes));
        protectedSecret.reset();
        const std::lock_guard<std::mutex> lock(mailbox->mutex);
        auto* receiver = mailbox->receiver;
        if (!receiver) return;
        QMetaObject::invokeMethod(receiver,
            [receiver, ref, target, remove, ok, persistent, finished = std::move(finished)] {
            receiver->_credentialOperationPending = false;
            receiver->_elevationPersistent = persistent;
            bool saved = false;
            if (ok) {
                if (remove) receiver->_elevationReferences.remove(ref);
                else receiver->_elevationReferences.insert(ref);
                receiver->revokeElevation(target);
                ++receiver->_elevationRevision;
                saved = receiver->save();
            }
            emit receiver->changed();
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
