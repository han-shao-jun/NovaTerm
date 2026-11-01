#include "FileResourceAdapter.h"
#include "transport/SshTransport.h"
namespace NovaTerm::Mcp {
FileResourceAdapter::FileResourceAdapter(SessionDirectory& sessions, QObject* parent)
    : QObject(parent), _sessions(sessions) {}
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
}
