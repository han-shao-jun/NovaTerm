#include "EvidenceStore.h"
#include <QCryptographicHash>
#include <QUuid>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace NovaTerm::Evidence {
namespace {
qint64 monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::optional<qsizetype> byteCount(const QVector<RawFragment>& fragments) {
    if (fragments.isEmpty() || fragments.size() > EvidenceStore::MaxFragments) return {};
    qsizetype count = 0;
    for (const auto& part : fragments) {
        if (part.byteCount > quint64(EvidenceStore::MaxBytes - count)
            || part.byteOffset > quint64(std::numeric_limits<qint64>::max()) - part.byteCount) return {};
        count += qsizetype(part.byteCount);
    }
    return count;
}
}
struct EvidenceStore::State {
    struct Entry { EvidenceRef ref; qint64 expires{0}; quint64 token{0}; };
    EvidenceAuthorization authorization;
    Clock clock;
    std::map<SourceKind, std::shared_ptr<const EvidenceSourceReader>> readers;
    std::map<QString, Entry> entries;
    qsizetype pending{0};
    quint64 revision{0};
    bool alive{true};
    void sweep() {
        const auto now = clock();
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->second.expires <= now) it = entries.erase(it); else ++it;
        }
    }
    bool allowed(const ResourceContext& context, const EvidenceRequest& request, AuthorizationPurpose purpose) const {
        const bool cross = context.principalId != request.source.ownerPrincipalId;
        if (!authorization || context.principalId.isEmpty() || context.tokenGeneration == 0
            || (cross && !authorization(context, request.source, request.fragments, AuthorizationPurpose::CrossPrincipal))) return false;
        return alive && authorization(context, request.source, request.fragments, purpose) && alive;
    }
};
QString errorCode(Error value) {
    switch (value) {
    case Error::None: return {};
    case Error::InvalidRequest: return "INVALID_ARGUMENT";
    case Error::PermissionDenied: return "PERMISSION_DENIED";
    case Error::EvidenceExpired: return "EVIDENCE_EXPIRED";
    case Error::LimitExceeded: return "EVIDENCE_LIMIT";
    case Error::PersistenceUnavailable: return "EVIDENCE_PERSISTENCE_UNAVAILABLE";
    }
    return "INVALID_ARGUMENT";
}
EvidenceStore::EvidenceStore(EvidenceAuthorization authorization, Clock clock) : _state(std::make_shared<State>()) {
    _state->authorization = std::move(authorization); _state->clock = clock ? std::move(clock) : monotonicMs;
}
EvidenceStore::~EvidenceStore() { _state->alive = false; _state->entries.clear(); }
void EvidenceStore::registerSource(SourceKind kind, std::shared_ptr<const EvidenceSourceReader> source) {
    _state->readers[kind] = std::move(source);
}
void EvidenceStore::createAsync(EvidenceRequest request, ResourceContext context, EvidenceCallback callback) {
    const auto state = _state;
    if (!callback) return;
    const auto fail = [&callback](Error value) { callback(EvidenceResult{value}); };
    if (request.persist) { fail(Error::PersistenceUnavailable); return; }
    const auto expected = byteCount(request.fragments);
    if (!expected) { fail(Error::LimitExceeded); return; }
    if (request.source.resourceId.isEmpty() || request.source.generation == 0
        || request.source.ownerPrincipalId.isEmpty() || request.ttlMs < 1 || request.ttlMs > 300000) {
        fail(Error::InvalidRequest); return;
    }
    if (!state->allowed(context, request, AuthorizationPurpose::Admission)) { fail(Error::PermissionDenied); return; }
    state->sweep();
    if (state->pending >= 32 || state->entries.size() + size_t(state->pending) >= size_t(MaxReferences)) { fail(Error::LimitExceeded); return; }
    const auto it = state->readers.find(request.source.kind);
    if (it == state->readers.end() || !it->second) { fail(Error::EvidenceExpired); return; }
    const auto source = it->second;
    const auto revision = state->revision;
    const qint64 expiry = state->clock() + request.ttlMs;
    ++state->pending;
    const std::weak_ptr<State> weak = state;
    source->readAsync(request.source, request.fragments, MaxBytes,
        [weak, source, request, context, callback = std::move(callback), revision, expiry, expected = *expected,
         completed = std::make_shared<bool>(false)](SourceReadResult data) mutable {
            if (std::exchange(*completed, true)) return;
            const auto current = weak.lock();
            if (!current || !current->alive) { callback(EvidenceResult{Error::EvidenceExpired}); return; }
            --current->pending;
            Error error = data.error;
            if (!current->allowed(context, request, AuthorizationPurpose::Delivery)) error = Error::PermissionDenied;
            else if (current->revision != revision || current->clock() >= expiry
                || current->readers[request.source.kind] != source || data.generation != request.source.generation)
                error = Error::EvidenceExpired;
            else if (data.bytes.size() != expected) error = Error::EvidenceExpired;
            if (error != Error::None) { callback(EvidenceResult{error}); return; }
            EvidenceRef ref;
            ref.evidenceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            ref.source = request.source; ref.fragments = request.fragments;
            ref.contentHash = QCryptographicHash::hash(data.bytes, QCryptographicHash::Sha256).toHex();
            ref.collectedAt = QDateTime::currentDateTimeUtc(); ref.expiresAt = ref.collectedAt.addMSecs(expiry - current->clock());
            ref.sourceCompleteness = data.sourceCompleteness; ref.projectionVersion = data.projectionVersion;
            ref.diagnostic = request.diagnostic;
            current->entries.emplace(ref.evidenceId, State::Entry{ref, expiry, context.tokenGeneration});
            callback(EvidenceResult{Error::None, ref, std::move(data.bytes), true});
        });
}
void EvidenceStore::resolveAsync(QString id, ResourceContext context, qsizetype maximum, EvidenceCallback callback) {
    const auto state = _state;
    if (!callback) return;
    state->sweep();
    const auto it = state->entries.find(id);
    if (it == state->entries.end()) { callback(EvidenceResult{Error::EvidenceExpired}); return; }
    const auto entry = it->second;
    EvidenceRequest request{entry.ref.source, entry.ref.fragments};
    if (!state->allowed(context, request, AuthorizationPurpose::Admission)
        || (context.principalId == entry.ref.source.ownerPrincipalId && context.tokenGeneration != entry.token)) {
        callback(EvidenceResult{Error::PermissionDenied}); return;
    }
    const auto expected = byteCount(request.fragments);
    if (!expected || maximum < *expected || maximum > MaxBytes || maximum < 0) { callback(EvidenceResult{Error::LimitExceeded}); return; }
    const auto reader = state->readers.find(request.source.kind);
    if (reader == state->readers.end() || !reader->second) { callback(EvidenceResult{Error::EvidenceExpired}); return; }
    const auto source = reader->second;
    if (state->pending >= 32) { callback(EvidenceResult{Error::LimitExceeded}); return; }
    ++state->pending;
    const std::weak_ptr<State> weak = state;
    source->readAsync(request.source, request.fragments, maximum,
        [weak, source, request, context, id, entry, callback = std::move(callback), expected = *expected,
         completed = std::make_shared<bool>(false)](SourceReadResult data) mutable {
            if (std::exchange(*completed, true)) return;
            const auto current = weak.lock();
            if (!current || !current->alive) { callback(EvidenceResult{Error::EvidenceExpired}); return; }
            --current->pending;
            Error error = data.error;
            if (!current->allowed(context, request, AuthorizationPurpose::Delivery)) error = Error::PermissionDenied;
            else if (!current->entries.count(id) || current->clock() >= entry.expires
                || current->readers[request.source.kind] != source || data.generation != request.source.generation
                || data.bytes.size() != expected || QCryptographicHash::hash(data.bytes, QCryptographicHash::Sha256).toHex() != entry.ref.contentHash)
                error = Error::EvidenceExpired;
            if (error != Error::None) { callback(EvidenceResult{error}); return; }
            callback(EvidenceResult{Error::None, entry.ref, std::move(data.bytes), true});
        });
}
void EvidenceStore::remove(const QString& id) { _state->entries.erase(id); }
void EvidenceStore::clear() { _state->entries.clear(); ++_state->revision; }
qsizetype EvidenceStore::size() const { _state->sweep(); return qsizetype(_state->entries.size()); }
}
