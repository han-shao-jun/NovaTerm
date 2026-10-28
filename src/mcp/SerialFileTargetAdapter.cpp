#include "SerialFileTargetAdapter.h"
#include "transport/SerialTransport.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopedValueRollback>

namespace NovaTerm::Mcp {
SerialFileTargetAdapter::SerialFileTargetAdapter(AccessStore& access, SessionDirectory& sessions, QObject* parent)
    : QObject(parent), _access(access), _sessions(sessions),
      _targets([this](const QString& client) { return _access.enabled() ? _access.identityVersion(client) : 0; })
{
    connect(&_targets, &SerialFileTargets::changed, this, &SerialFileTargetAdapter::changed);
    connect(&_access, &AccessStore::revoked, this, [this](const QString& client, const QString& session) {
        if (session.isEmpty()) _targets.revoke(client);
    });
    connect(&_sessions, &SessionDirectory::invalidated, this, [this] { pruneDisconnected(); });
    connect(&_sessions, &SessionDirectory::changed, this, [this] { pruneDisconnected(); });
}
std::optional<SerialFileBinding> SerialFileTargetAdapter::liveBinding(const QString& id)
{
    const auto entry = _sessions.find(id);
    if (!entry || !entry->session || entry->kind != TransportKind::Serial
        || entry->state != SessionState::Running || entry->session->state() != SessionState::Running
        || entry->session->transport() != entry->transport || entry->session->statistics().generation != entry->generation)
        return std::nullopt;
    const auto* serial = qobject_cast<SerialTransport*>(entry->transport.data());
    if (!serial || !serial->isConnected() || serial->hasPendingDisconnect()
        || !serial->config().isValid() || serial->config().dataBits != QSerialPort::Data8) return std::nullopt;
    const auto io = serial->ioStatistics();
    if (!io.generation || !io.connected || !io.desiredOpen || io.closePending || io.stopping) return std::nullopt;
    const auto& config = serial->config();
    const QJsonArray fields{config.portName, config.baudRate, int(config.dataBits), int(config.parity),
        int(config.stopBits), int(config.flowControl)};
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(fields).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    std::optional<QString> fingerprint;
    if (!entry->targetFingerprint.isEmpty()) fingerprint = entry->targetFingerprint;
    return SerialFileBinding{{SessionId{entry->id}, AttachmentId{entry->attachmentId}, Epoch{entry->epoch},
        Generation{entry->generation}, fingerprint}, io.generation, digest};
}
void SerialFileTargetAdapter::pruneDisconnected()
{
    if (_pruning) return;
    const QScopedValueRollback<bool> pruning(_pruning, true);
    // find() may synchronously replace or revoke grants; only invalidate this old binding snapshot.
    const auto bindings = _targets.bindings();
    for (const auto& old : bindings) {
        const auto current = liveBinding(old.connection.session.str());
        if (!current || !SerialFileTargets::sameBinding(old, *current)) _targets.invalidateBinding(old);
    }
}
std::optional<SerialFileTarget> SerialFileTargetAdapter::share(const QString& client, quint64 identity,
    const SerialFileBinding& expected, const QString& title)
{
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return std::nullopt;
    pruneDisconnected();
    const auto binding = liveBinding(expected.connection.session.str());
    if (!binding || !SerialFileTargets::sameBinding(expected, *binding)) return std::nullopt;
    const auto target = _targets.share(client, identity, *binding, title);
    if (!target) return std::nullopt;
    const auto current = liveBinding(expected.connection.session.str());
    if (!current || !SerialFileTargets::sameBinding(expected, *current)) {
        _targets.invalidateBinding(expected);
        return std::nullopt;
    }
    for (const auto& grant : _targets.list(client, identity))
        if (grant.id == target->id && grant.revision == target->revision) return grant;
    return std::nullopt;
}
Result<QList<SerialFileTarget>> SerialFileTargetAdapter::list(const QString& client, quint64 identity)
{
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return Error{ErrorCode::PermissionDenied};
    pruneDisconnected();
    QList<SerialFileTarget> candidates;
    const auto snapshot = _targets.list(client, identity);
    for (const auto& candidate : snapshot) {
        const auto binding = liveBinding(candidate.binding.connection.session.str());
        if (!binding || !SerialFileTargets::sameBinding(candidate.binding, *binding)) {
            _targets.invalidateBinding(candidate.binding);
            continue;
        }
        candidates.append(candidate);
    }
    if (!_access.enabled() || !identity || _access.identityVersion(client) != identity) return Error{ErrorCode::PermissionDenied};
    QList<SerialFileTarget> result;
    const auto current = _targets.list(client, identity);
    // Re-read authorization after every synchronous directory callback has completed.
    for (const auto& candidate : candidates)
        for (const auto& grant : current)
            if (grant.id == candidate.id && grant.revision == candidate.revision
                && SerialFileTargets::sameBinding(grant.binding, candidate.binding)) result.append(grant);
    return result;
}
}
