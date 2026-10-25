#include "ResourceSharingService.h"
#include "historysearch/HistoryAuthorizationStore.h"
#include "historysearch/HistoryReadService.h"
#include "historysearch/HistorySearchService.h"
#include "evidence/EvidenceStore.h"
#include "evidence/ImportedResourceStore.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QPointer>
#include <algorithm>
#include <map>
#include <set>
namespace NovaTerm::Resources {
namespace R = Recording;
namespace E = Evidence;
namespace {
QString completeness(R::Integrity value) {
    switch (value) {
    case R::Integrity::Lossless: return "lossless";
    case R::Integrity::KnownGap: return "known_gap";
    case R::Integrity::UnknownTail: return "unknown_tail";
    case R::Integrity::RecoveredUnsealed: return "recovered_unsealed";
    }
    return "unknown_tail";
}
class HistoryEvidenceReader final : public E::EvidenceSourceReader {
public:
    using Check = std::function<bool(const E::ResourceBinding&, const R::HistoryRecord&)>;
    HistoryEvidenceReader(R::HistoryReadPort port, Check check) : _port(std::move(port)), _check(std::move(check)) {}
    void readAsync(const E::ResourceBinding& binding, const QVector<E::RawFragment>& fragments, qsizetype maximum,
                   std::function<void(E::SourceReadResult)> callback) const override {
        const R::HistoryCaptureId id{QUuid(binding.resourceId)};
        const auto resource = _port.describe(id);
        if (!resource || resource->generation != binding.generation) { callback({E::Error::EvidenceExpired}); return; }
        struct Read final : std::enable_shared_from_this<Read> {
            R::HistoryReadPort port;
            Check check;
            E::ResourceBinding binding;
            QVector<E::RawFragment> fragments;
            qsizetype maximum{0};
            std::function<void(E::SourceReadResult)> done;
            std::map<quint64, R::HistoryRecord> records;
            std::vector<quint64> ids;
            size_t index{0};
            qsizetype bytes{0};
            void next() {
                if (index == ids.size()) {
                    E::SourceReadResult result;
                    const auto resourceNow = port.describe({QUuid(binding.resourceId)});
                    if (!resourceNow || resourceNow->generation != binding.generation) { done({E::Error::EvidenceExpired}); return; }
                    for (const auto& part : fragments) {
                        const auto& record = records.at(part.recordId);
                        if (!check(binding, record)) { done({E::Error::PermissionDenied}); return; }
                        if (record.kind != R::RecordKind::Payload || part.byteOffset > quint64(record.payload.size())
                            || part.byteCount > quint64(record.payload.size()) - part.byteOffset
                            || part.byteCount > quint64(maximum - result.bytes.size())) { done({E::Error::EvidenceExpired}); return; }
                        result.bytes += record.payload.mid(qsizetype(part.byteOffset), qsizetype(part.byteCount));
                    }
                    result.generation = binding.generation; result.sourceCompleteness = completeness(resourceNow->capture.integrity);
                    result.projectionVersion = 1; done(std::move(result)); return;
                }
                R::HistoryRange range; range.firstRecordId = ids[index]; range.lastRecordId = ids[index]; range.maxRecords = 1;
                const auto self = shared_from_this();
                if (!port.readCommitted({QUuid(binding.resourceId)}, range, [self](R::ReadResult result) {
                    if (result.missing || !result.error.isEmpty() || result.records.size() != 1
                        || result.records.front().recordId != self->ids[self->index]) { self->done({E::Error::EvidenceExpired}); return; }
                    self->bytes += result.records.front().payload.size();
                    if (self->bytes > 1024 * 1024) { self->done({E::Error::LimitExceeded}); return; }
                    self->records.emplace(self->ids[self->index++], std::move(result.records.front())); self->next();
                })) done({E::Error::LimitExceeded});
            }
        };
        auto read = std::make_shared<Read>(); read->port = _port; read->check = _check;
        read->binding = binding; read->fragments = fragments; read->maximum = maximum; read->done = std::move(callback);
        std::set<quint64> ids;
        for (const auto& part : fragments) ids.insert(part.recordId);
        read->ids.assign(ids.begin(), ids.end()); read->next();
    }
private:
    R::HistoryReadPort _port;
    Check _check;
};
}
struct ResourceSharingService::Impl {
    IdentityLookup identity;
    GrantPersistence persistence;
    R::HistoryReadPort source, port;
    R::HistoryAuthorizationStore authorization;
    std::unique_ptr<R::HistoryReadService> reads;
    std::unique_ptr<R::HistorySearchService> searches;
    std::unique_ptr<E::EvidenceStore> evidence;
    std::unique_ptr<E::ImportedResourceStore> imports;
    std::map<QString, R::ResourceContext> importedOwners;
    struct ImportBatch { bool revoked{false}; size_t pending{0}; };
    std::map<QString, std::shared_ptr<ImportBatch>> pendingImports;
    struct CachedPage { EvidenceSearchPage result; qint64 expires; };
    std::map<QString, CachedPage> pages;
    QElapsedTimer clock;
    R::ResourceContext context(const QString& principal) const { return {principal, identity ? identity(principal) : 0}; }
    bool valid(const R::ResourceContext& context) const {
        return !context.principalId.isEmpty() && context.tokenEpoch && this->context(context.principalId).tokenEpoch == context.tokenEpoch;
    }
    std::optional<R::HistoryResource> describe(R::HistoryCaptureId id) const { return source.describe ? source.describe(id) : std::nullopt; }
    bool save(const std::vector<R::ResourceGrant>& grants) const {
        if (!persistence.save) return false;
        std::vector<R::ResourceGrant> persisted;
        for (const auto& grant : grants) if (grant.persistent && valid({grant.principalId, grant.tokenEpoch})) persisted.push_back(grant);
        return persistence.save(persisted);
    }
};
ResourceSharingService::ResourceSharingService(IdentityLookup identity, GrantPersistence persistence, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>()) {
    auto& p = *_impl; p.identity = std::move(identity); p.persistence = std::move(persistence); p.clock.start();
    if (p.persistence.load) {
        const auto grants = p.persistence.load();
        if (grants.size() <= 128) for (const auto& grant : grants)
            if (p.valid({grant.principalId, grant.tokenEpoch})) p.authorization.setGrant(grant);
    }
    const QPointer<ResourceSharingService> self(this);
    p.port.describe = [self](R::HistoryCaptureId id) -> std::optional<R::HistoryResource> { return self ? self->describe(id) : std::nullopt; };
    p.port.readCommitted = [self](R::HistoryCaptureId id, R::HistoryRange range, auto done) {
        if (!self) return false;
        const auto read = self->_impl->source.readCommitted;
        return read && read(id, range, std::move(done));
    };
    p.evidence = std::make_unique<E::EvidenceStore>([self](const E::ResourceContext& context, const E::ResourceBinding& binding,
        const QVector<E::RawFragment>& fragments, E::AuthorizationPurpose purpose) {
        if (!self || purpose == E::AuthorizationPurpose::CrossPrincipal || context.principalId != binding.ownerPrincipalId
            || !self->_impl->valid({context.principalId, context.tokenGeneration})) return false;
        if (binding.kind == E::SourceKind::ImportedFile) {
            const auto owner = self->_impl->importedOwners.find(binding.resourceId);
            return owner != self->_impl->importedOwners.end() && owner->second.principalId == context.principalId
                && owner->second.tokenEpoch == context.tokenGeneration;
        }
        const auto grant = self->authorize({context.principalId, context.tokenGeneration}, {QUuid(binding.resourceId)}, binding.generation);
        if (!grant) return false;
        for (const auto& part : fragments)
            if (part.recordId < grant->firstRecordId || (grant->lastRecordId && part.recordId > grant->lastRecordId)) return false;
        return true;
    });
    p.imports = std::make_unique<E::ImportedResourceStore>();
    p.evidence->registerSource(E::SourceKind::ImportedFile, p.imports->reader());
    setHistorySource({});
}
ResourceSharingService::~ResourceSharingService() = default;
void ResourceSharingService::setHistorySource(R::HistoryReadPort source) {
    auto& p = *_impl; p.reads.reset(); p.searches.reset(); p.pages.clear(); p.source = std::move(source);
    p.reads = std::make_unique<R::HistoryReadService>(p.port, p.authorization);
    p.searches = std::make_unique<R::HistorySearchService>(p.port, p.authorization);
    const QPointer<ResourceSharingService> self(this);
    p.evidence->registerSource(E::SourceKind::TerminalCapture, std::make_shared<HistoryEvidenceReader>(p.port,
        [self](const E::ResourceBinding& binding, const R::HistoryRecord& record) {
            if (!self) return false;
            const auto grant = self->authorize(self->context(binding.ownerPrincipalId), record.captureId, binding.generation);
            return grant && R::HistoryAuthorizationStore::allowsRecord(*grant, record);
        }));
}
R::ResourceContext ResourceSharingService::context(const QString& principal) const { return _impl->context(principal); }
std::optional<R::ResourceGrant> ResourceSharingService::authorize(const R::ResourceContext& context, R::HistoryCaptureId id, quint64 generation) const {
    return _impl->valid(context) ? _impl->authorization.authorize(context, id, generation) : std::nullopt;
}
std::optional<R::HistoryResource> ResourceSharingService::describe(R::HistoryCaptureId id) const { return _impl->describe(id); }
CaptureListPage ResourceSharingService::list(const R::ResourceContext& context, const CaptureListQuery& request) const {
    CaptureListPage page;
    if (!_impl->valid(context)) { page.error = "PERMISSION_DENIED"; return page; }
    const auto grants = _impl->authorization.grants(context); QString signature;
    for (const auto& grant : grants) signature += grant.captureId.text() + ':' + QString::number(grant.accessVersion) + ';';
    page.digest = QString::fromLatin1(QCryptographicHash::hash(signature.toUtf8(), QCryptographicHash::Sha256).toHex());
    if (!request.digest.isEmpty() && request.digest != page.digest) { page.error = "CURSOR_STALE"; return page; }
    if (request.limit < 1 || request.limit > 200) { page.error = "INVALID_ARGUMENT"; return page; }
    for (const auto& grant : grants) {
        if (grant.captureId.text() <= request.after) continue;
        const auto resource = describe(grant.captureId); if (!resource) continue;
        if (page.captures.size() >= size_t(request.limit)) { page.nextAfter = page.captures.back().id.text(); break; }
        const auto& capture = resource->capture;
        page.captures.push_back({capture.id, capture.createdMs, capture.highWaterRecordId, grant.firstRecordId,
            grant.lastRecordId, capture.integrity, capture.active, grant.persistent});
    }
    return page;
}
void ResourceSharingService::readAsync(R::HistoryReadRequest request, R::ResourceContext context, std::function<void(R::HistoryReadPage)> done) {
    if (!_impl->valid(context)) { R::HistoryReadPage page; page.error = "PERMISSION_DENIED"; done(std::move(page)); return; }
    const QPointer<ResourceSharingService> self(this);
    request.admitted = [self, context, admitted = std::move(request.admitted)] {
        return self && self->_impl->valid(context) && (!admitted || admitted());
    };
    _impl->reads->readAsync(std::move(request), context, [self, context, done = std::move(done)](R::HistoryReadPage page) mutable {
        if (!self) return;
        if (!self->_impl->valid(context)) { page = {}; page.error = "PERMISSION_DENIED"; }
        done(std::move(page));
    });
}
void ResourceSharingService::createEvidence(E::EvidenceRequest request, R::ResourceContext context, E::EvidenceCallback done) {
    _impl->evidence->createAsync(std::move(request), {context.principalId, context.tokenEpoch}, std::move(done));
}
void ResourceSharingService::resolveEvidence(QString id, R::ResourceContext context, qsizetype maximum, E::EvidenceCallback done) {
    _impl->evidence->resolveAsync(std::move(id), {context.principalId, context.tokenEpoch}, maximum, std::move(done));
}
void ResourceSharingService::searchAsync(R::HistorySearchRequest request, R::ResourceContext context, std::function<void(EvidenceSearchPage)> done) {
    if (!_impl->valid(context)) { EvidenceSearchPage page; page.page.error = "PERMISSION_DENIED"; done(std::move(page)); return; }
    const auto resource = describe(request.captureId);
    const auto generation = resource ? resource->generation : 0;
    const auto grant = authorize(context, request.captureId, generation);
    QString key;
    if (grant && !request.cursor.isEmpty()) key = context.principalId + '/' + QString::number(context.tokenEpoch)
        + '/' + QString::number(grant->accessVersion) + '/' + request.captureId.text() + '/' + request.cursor
        + '/' + QString::number(int(request.mode)) + '/' + QString::number(request.maxMatches) + '/' + QString::number(request.maxResponseBytes)
        + '/' + QString::fromLatin1(QCryptographicHash::hash(request.query, QCryptographicHash::Sha256).toHex());
    for (auto it = _impl->pages.begin(); it != _impl->pages.end();) {
        if (it->second.expires <= _impl->clock.elapsed()) it = _impl->pages.erase(it); else ++it;
    }
    const QPointer<ResourceSharingService> self(this);
    const auto capture = request.captureId;
    const auto cursor = request.cursor;
    const auto cancelled = request.cancelled;
    request.admitted = [self, context, capture, generation, version = grant ? grant->accessVersion : 0, admitted = std::move(request.admitted)] {
        if (!self || !self->_impl->valid(context)) return false;
        const auto current = self->authorize(context, capture, generation);
        return current && current->accessVersion == version && (!admitted || admitted());
    };
    const auto admitted = request.admitted;
    _impl->searches->searchAsync(std::move(request), context, [self, context, capture, generation, cursor, key, cancelled, admitted, done = std::move(done)](R::HistorySearchPage page) mutable {
        if (!self) return;
        if (!page.error.isEmpty()) { done({std::move(page), {}}); return; }
        const bool allowed = !admitted || admitted();
        if (!self) return;
        if (!allowed || (cancelled && cancelled->load())) {
            const auto token = page.nextCursor.isEmpty() ? cursor : page.nextCursor;
            if (!token.isEmpty()) self->_impl->searches->cancel(token, context);
            page = {}; page.error = self->_impl->valid(context) ? "CANCELLED" : "PERMISSION_DENIED";
        }
        if (!page.error.isEmpty()) { done({std::move(page), {}}); return; }
        const auto prior = self->_impl->pages.find(key);
        if (!key.isEmpty() && prior != self->_impl->pages.end()) { done(prior->second.result); return; }
        const bool hasHits = !page.hits.empty();
        E::EvidenceRequest evidence; evidence.source = {E::SourceKind::TerminalCapture, capture.text(), generation, context.principalId};
        for (const auto& hit : page.hits) for (const auto& part : hit.ranges)
            evidence.fragments.append({part.recordId, quint64(part.byteOffset), quint64(part.byteCount)});
        auto deliver = [self, context, cursor, key, cancelled, admitted, page = std::move(page), done = std::move(done)](E::EvidenceResult result) mutable {
            if (!self) return;
            const bool allowed = !admitted || admitted();
            if (!self) return;
            if (!allowed || (cancelled && cancelled->load())) {
                const auto token = page.nextCursor.isEmpty() ? cursor : page.nextCursor;
                if (!token.isEmpty()) self->_impl->searches->cancel(token, context);
                if (!result.reference.evidenceId.isEmpty()) self->_impl->evidence->remove(result.reference.evidenceId);
                page = {}; page.error = "CANCELLED";
            } else if (!page.hits.empty() && result.error != E::Error::None) {
                if (!page.nextCursor.isEmpty()) self->_impl->searches->cancel(page.nextCursor, context);
                page = {}; page.error = E::errorCode(result.error);
            }
            EvidenceSearchPage delivery{std::move(page), {}};
            if (delivery.page.error.isEmpty()) delivery.evidence = std::move(result.reference);
            if (delivery.page.error.isEmpty() && !key.isEmpty() && self->_impl->pages.size() < 16)
                self->_impl->pages[key] = {delivery, self->_impl->clock.elapsed() + 60000};
            done(std::move(delivery));
        };
        if (!hasHits) { deliver({}); return; }
        self->_impl->evidence->createAsync(std::move(evidence), {context.principalId, context.tokenEpoch}, std::move(deliver));
    });
}
void ResourceSharingService::importFile(QString client, QString path, std::function<void(ImportResult)> callback) {
    const auto ctx = _impl->context(client);
    if (!_impl->imports || !ctx.tokenEpoch) { callback({"Select a registered client before sharing a file."}); return; }
    auto& pending = _impl->pendingImports[client];
    if (!pending) pending = std::make_shared<Impl::ImportBatch>();
    const auto batch = pending; ++batch->pending;
    const QPointer<ResourceSharingService> self(this);
    _impl->imports->shareLocalFileAsync({std::move(path)}, client, [self, ctx, batch, callback = std::move(callback)](E::ImportedResource result) {
        if (!self) return;
        --batch->pending;
        const auto current = self->_impl->pendingImports.find(ctx.principalId);
        if (!batch->pending && current != self->_impl->pendingImports.end() && current->second == batch)
            self->_impl->pendingImports.erase(current);
        if (batch->revoked) {
            if (self->_impl->imports) self->_impl->imports->remove(result.binding.resourceId);
            callback({"File sharing was revoked while importing."}); return;
        }
        if (self->_impl->context(ctx.principalId).tokenEpoch != ctx.tokenEpoch) {
            self->_impl->imports->remove(result.binding.resourceId); callback({"Client identity changed while sharing the file."}); return;
        }
        if (result.error != E::Error::None) { callback({E::errorCode(result.error)}); return; }
        self->_impl->importedOwners[result.binding.resourceId] = ctx;
        callback({{}, result.binding.resourceId, QString::fromLatin1(result.contentHash), result.byteCount});
    });
}
void ResourceSharingService::clearImportedFiles(const QString& client) {
    const auto pending = _impl->pendingImports.find(client);
    if (pending != _impl->pendingImports.end()) {
        pending->second->revoked = true; _impl->pendingImports.erase(pending);
    }
    for (auto it = _impl->importedOwners.begin(); it != _impl->importedOwners.end();) {
        if (it->second.principalId == client) { _impl->imports->remove(it->first); it = _impl->importedOwners.erase(it); }
        else ++it;
    }
}
QString ResourceSharingService::share(const QString& client, R::HistoryCaptureId id, quint64 first, quint64 last, bool persistent) {
    auto& p = *_impl;
    const auto resource = p.describe(id); const auto ctx = p.context(client);
    if (!p.persistence.save) return "History grants are locked by another process or their private directory is unavailable.";
    if (!resource || !ctx.tokenEpoch || !first || (last && last < first)) return "Invalid capture, client or record range.";
    R::ResourceGrant grant; grant.principalId = client; grant.tokenEpoch = ctx.tokenEpoch; grant.captureId = id;
    grant.resourceGeneration = resource->generation; grant.firstRecordId = first; grant.lastRecordId = last; grant.persistent = persistent;
    std::vector<R::ResourceGrant> proposed, retired;
    for (const auto& existing : p.authorization.allGrants()) {
        const auto current = p.describe(existing.captureId);
        if (!current || current->generation != existing.resourceGeneration
            || p.context(existing.principalId).tokenEpoch != existing.tokenEpoch) retired.push_back(existing);
        else if (existing.principalId != client || existing.captureId != id) proposed.push_back(existing);
    }
    if (proposed.size() >= 128) return "History grant limit reached.";
    proposed.push_back(grant);
    if (!p.save(proposed)) return "Could not save history permissions; the grant was not changed.";
    for (const auto& expired : retired) p.authorization.revoke(expired.principalId, expired.captureId);
    p.authorization.setGrant(grant); p.pages.clear(); return {};
}
QString ResourceSharingService::revoke(const QString& client, R::HistoryCaptureId id) {
    auto& p = *_impl; auto proposed = p.authorization.allGrants();
    proposed.erase(std::remove_if(proposed.begin(), proposed.end(), [&client, id](const auto& value) { return value.principalId == client && value.captureId == id; }), proposed.end());
    if (!p.save(proposed)) return "Could not save the revocation; history permission is still granted.";
    p.authorization.revoke(client, id); p.pages.clear(); return {};
}
QString ResourceSharingService::sharingStatus(const QString& client, R::HistoryCaptureId id) const {
    const auto& p = *_impl; const auto resource = describe(id);
    const auto grant = resource ? authorize(p.context(client), id, resource->generation) : std::nullopt;
    return grant ? QString("Shared records %1–%2 (%3)").arg(grant->firstRecordId).arg(grant->lastRecordId ? QString::number(grant->lastRecordId) : "committed high water").arg(grant->persistent ? "persistent" : "until exit") : QString("Not shared");
}

}
