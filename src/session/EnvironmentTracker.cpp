#include "EnvironmentTracker.h"
namespace NovaTerm::Session {
using namespace Capabilities;
EnvironmentTracker::EnvironmentTracker(Now now) : _now(std::move(now)) {}
void EnvironmentTracker::invalidate()
{
    _outputTail.clear();
    EnvironmentSnapshot invalidated{};
    invalidated.binding = _snapshot.binding;
    invalidated.revision = _snapshot.revision + 1;
    _snapshot = std::move(invalidated);
}
void EnvironmentTracker::newConnection(SessionBinding binding)
{
    invalidate();
    _snapshot.binding = std::move(binding);
    _outputTail.clear();
    _adapter.clear();
    _adapterSequence = 0;
}
void EnvironmentTracker::declareEnvironment(EnvironmentKind kind)
{
    invalidate();
    _snapshot.kind = kind;
    _snapshot.source = EnvironmentSource::UserDeclaration;
    _snapshot.observedAt = _now();
    _snapshot.validUntil = _snapshot.observedAt + EvidenceLifetime;
}
void EnvironmentTracker::observeOutput(QByteArrayView bytes)
{
    if (_snapshot.kind == EnvironmentKind::Unknown) return;
    // A small overlap detects revocation boundaries split across input chunks.
    // Candidate text never establishes an environment or renews readiness.
    const auto prefix = bytes.first((std::min)(bytes.size(), qsizetype(128)));
    const auto inspect = [this](QByteArrayView text) {
        // Read all candidate flags before invalidate() can clear the viewed overlap.
        const bool boot = text.contains("U-Boot"), linuxCandidate = text.contains("Linux version");
        const bool boundary = text.contains("login:") || text.contains("Password:") || text.contains(">>> ")
            || text.contains("\x1b[?1049h") || text.contains("\x1b[?47h");
        if (boundary) { invalidate(); return; }
        if (boot) observeCandidate(EnvironmentKind::Bootloader);
        if (linuxCandidate) observeCandidate(EnvironmentKind::Linux);
    };
    _outputTail.append(prefix.data(), prefix.size());
    inspect(_outputTail);
    inspect(bytes);
    if (_snapshot.kind == EnvironmentKind::Unknown) return;
    if (bytes.size() >= 128) _outputTail = QByteArray(bytes.last(128));
    else _outputTail = _outputTail.right(128);
}
void EnvironmentTracker::observeCandidate(EnvironmentKind kind)
{
    // Ordinary output can revoke evidence, never create or renew it.
    if (_snapshot.kind != EnvironmentKind::Unknown && kind != _snapshot.kind)
        invalidate();
}
void EnvironmentTracker::pairAdapter(QString adapter)
{
    invalidate();
    if (_adapter != adapter) _adapterSequence = 0;
    _adapter = std::move(adapter);
}
bool EnvironmentTracker::acceptAdapter(const AdapterEnvironmentReport& report)
{
    if (_adapter.isEmpty() || report.adapter != _adapter || report.sequence <= _adapterSequence
        || !sameResource(report.binding, _snapshot.binding)) return false;
    _adapterSequence = report.sequence;
    invalidate();
    _snapshot.kind = report.kind;
    _snapshot.source = EnvironmentSource::PairedAdapter;
    _snapshot.observedAt = _now();
    _snapshot.validUntil = _snapshot.observedAt + EvidenceLifetime;
    _snapshot.readinessEvidenceId = report.readinessEvidenceId;
    _snapshot.ready = report.ready && !report.readinessEvidenceId.isEmpty()
        && report.kind != EnvironmentKind::Unknown;
    return true;
}
void EnvironmentTracker::confirmOnce(EnvironmentKind kind, PrincipalId principal, CapabilityId recipe, QString digest)
{
    declareEnvironment(kind);
    _snapshot.source = EnvironmentSource::LocalConfirmation;
    _snapshot.readinessEvidenceId = QString::number(++_evidenceSequence);
    _snapshot.ready = kind != EnvironmentKind::Unknown && !principal.empty()
        && !recipe.empty() && !digest.isEmpty();
    _snapshot.oneSubmission = true;
    _snapshot.consentPrincipal = std::move(principal);
    _snapshot.consentRecipe = std::move(recipe);
    _snapshot.planDigest = std::move(digest);
}
bool EnvironmentTracker::consumeReadiness(quint64 revision, const QString& evidence)
{
    const auto current = snapshot();
    if (current.revision != revision || !current.ready || evidence.isEmpty()
        || current.readinessEvidenceId != evidence) return false;
    if (current.oneSubmission) invalidate();
    return true;
}
EnvironmentSnapshot EnvironmentTracker::snapshot()
{
    if (_snapshot.kind != EnvironmentKind::Unknown && _now() >= _snapshot.validUntil)
        invalidate();
    return _snapshot;
}
}
