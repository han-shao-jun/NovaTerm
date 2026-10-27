#include "TargetSessionSharing.h"
#include "session/SessionInputArbiter.h"
#include <QScopedValueRollback>

namespace NovaTerm::Mcp {
TargetSessionSharing::TargetSessionSharing(AccessStore& access, SessionDirectory& sessions,
    SavedTargets& targets, QObject* parent)
    : QObject(parent), _access(access), _sessions(sessions), _targets(targets)
{
    _timer.setInterval(100);
    connect(&_timer, &QTimer::timeout, this, [this] { process(); });
    connect(&_sessions, &SessionDirectory::changed, this, [this] { schedule(); });
    connect(&_targets, &SavedTargets::changed, this, [this] { schedule(); });
    connect(&_sessions, &SessionDirectory::invalidated, this, [this](const QString& id) {
        const auto entry = _sessions.find(id);
        for (qsizetype i = _pending.size(); i-- > 0;) {
            const auto& pending = _pending[i];
            if (pending->sessionId == id && (!entry || entry->session != pending->session
                || entry->transport != pending->transport || entry->generation != pending->generation
                || (!pending->fingerprint.isEmpty() && entry->targetFingerprint != pending->fingerprint)))
                _pending.removeAt(i);
        }
    });
    connect(&_access, &AccessStore::revoked, this, [this](const QString& client, const QString& session) {
        for (qsizetype i = _pending.size(); i-- > 0;)
            if ((client.isEmpty() || _pending[i]->client == client)
                && (session.isEmpty() || _pending[i]->sessionId == session))
                _pending.removeAt(i);
    });
}
void TargetSessionSharing::clear() { _pending.clear(); _timer.stop(); }
void TargetSessionSharing::schedule()
{
    if (_scheduled || _pending.isEmpty()) return;
    _scheduled = true;
    QTimer::singleShot(0, this, [this] { _scheduled = false; process(); });
}
void TargetSessionSharing::created(const QString& client, quint64 identity, const TargetConnectionRequest& request,
    const TargetConnectionFacts& facts, const NewSessionSharing& sharing)
{
    if (!sharing.read || sharing.revision.isEmpty() || _pending.size() >= 256) return;
    const auto entry = _sessions.find(facts.sessionId);
    if (!entry || !entry->session || !entry->transport) return;
    _pending.append(std::make_shared<const Pending>(Pending{client, request.targetId, request.configVersion,
        entry->id, entry->targetFingerprint, identity, entry->generation, sharing, entry->session, entry->transport}));
    _timer.start();
    schedule();
}
void TargetSessionSharing::process()
{
    if (_processing) return;
    const QScopedValueRollback<bool> processing(_processing, true);
    // Stable tickets survive synchronous callbacks; membership is the cancellation fact.
    const auto work = _pending;
    for (const auto& pending : std::as_const(work)) {
        const auto valid = [this, &pending] {
            return _pending.contains(pending) && !pending->deadline.hasExpired() && _access.enabled()
                && _access.identityVersion(pending->client) == pending->identityVersion
                && _targets.openingSource(pending->client, pending->identityVersion,
                    pending->target, pending->configVersion).has_value()
                && _targets.newSessionSharing(pending->client, pending->identityVersion,
                    pending->target).revision == pending->sharing.revision
                && pending->session && pending->transport
                && pending->session->transport() == pending->transport
                && pending->session->statistics().generation == pending->generation
                && !pending->session->manualConnectionGeneration()
                && !pending->session->inputArbiter()->userInputGeneration()
                && !pending->session->core()->localInputGeneration();
        };
        if (!valid()) { _pending.removeOne(pending); continue; }
        const auto entry = _sessions.find(pending->sessionId);
        if (!entry || !valid() || entry->transport != pending->transport || entry->generation != pending->generation
            || _access.grantSnapshot(ClientId{pending->client}, *entry).revision.value()) {
            _pending.removeOne(pending); continue;
        }
        if (entry->state == SessionState::Connecting) continue;
        if (entry->state != SessionState::Running || !pending->transport->isConnected()
            || pending->transport->hasPendingDisconnect()) { _pending.removeOne(pending); continue; }
        if (pending->transport->property("novatermHostKeyDecisionPending").toBool()) continue;
        PermissionSelection selection;
        selection.read = true;
        selection.fullTerminalInput = pending->sharing.input;
        selection.clearTerminal = pending->sharing.clear;
        _pending.removeOne(pending);
        static_cast<void>(_access.initializeSessionSharing(ClientId{pending->client}, selection, *entry));
    }
    if (_pending.isEmpty()) _timer.stop();
}
}
