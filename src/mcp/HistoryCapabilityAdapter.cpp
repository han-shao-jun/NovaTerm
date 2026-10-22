#include "HistoryCapabilityAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "history/HistoryService.h"
#include "history/HistoryReadService.h"
#include "history/HistorySearchService.h"
#include "evidence/EvidenceStore.h"
#include "evidence/ImportedResourceStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QPointer>
#include <QTimer>
#include <algorithm>
#include <map>
#include <set>

namespace NovaTerm::Mcp {
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
QJsonValue nullable(const QString& value) { return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value); }
QJsonObject boundedSuccess(QJsonObject data) {
    auto result = success(data);
    if (QJsonDocument(toolResult(result)).toJson(QJsonDocument::Compact).size() > 65536)
        return failure("RESPONSE_TOO_LARGE", "History response exceeds its encoded budget.");
    return result;
}
QJsonObject rawRange(const R::HistoryRawRange& part) {
    return {{"recordId", QString::number(part.recordId)}, {"byteOffset", double(part.byteOffset)}, {"byteCount", double(part.byteCount)}};
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
struct HistoryCapabilityAdapter::Impl {
    HistoryCapabilityAdapter* owner;
    AccessStore& access;
    QString path;
    std::unique_ptr<QLockFile> lock;
    bool writable{false};
    QPointer<R::HistoryService> source;
    R::HistoryAuthorizationStore authorization;
    R::HistoryReadPort port;
    std::unique_ptr<R::HistoryReadService> reads;
    std::unique_ptr<R::HistorySearchService> searches;
    std::unique_ptr<E::EvidenceStore> evidence;
    std::unique_ptr<E::ImportedResourceStore> imports;
    std::map<QString, R::ResourceContext> importedOwners;
    QByteArray cursorKey{randomBytes()};
    QElapsedTimer clock;
    struct CachedPage { QJsonObject result; qint64 expires; };
    std::map<QString, CachedPage> pages;
    Impl(HistoryCapabilityAdapter* o, AccessStore& a, QString state) : owner(o), access(a), path(QDir(state).filePath("history-grants.json")) {
        clock.start();
        if (!secureDirectory(state)) return;
        lock = std::make_unique<QLockFile>(QDir(state).filePath("history-grants.lock")); lock->setStaleLockTime(0);
        writable = lock->tryLock();
        if (!writable) return; // A second process must not resurrect stale on-disk grants.
        const auto saved = readJson(path);
        if (!saved) return;
        const auto rows = saved->value("grants").toArray();
        if (rows.size() > 128) return;
        for (const auto& value : rows) {
            const auto row = value.toObject(); R::ResourceGrant grant;
            grant.principalId = row.value("principal").toString(); grant.tokenEpoch = row.value("tokenEpoch").toString().toULongLong();
            grant.captureId = {QUuid(row.value("captureId").toString())}; grant.resourceGeneration = row.value("generation").toString().toULongLong();
            grant.firstRecordId = row.value("first").toString().toULongLong(); grant.lastRecordId = row.value("last").toString().toULongLong(); grant.persistent = true;
            if (context(grant.principalId).tokenEpoch == grant.tokenEpoch && grant.tokenEpoch) authorization.setGrant(grant);
        }
    }
    R::ResourceContext context(const QString& client) const {
        for (const auto& record : access.clients()) if (record.id.str() == client) {
            auto epoch = record.digest.toHex().left(16).toULongLong(nullptr, 16);
            return {client, epoch ? epoch : 1};
        }
        return {client, 0};
    }
    std::optional<R::HistoryResource> describe(R::HistoryCaptureId id) const {
        if (source) for (const auto& capture : source->captures()) if (capture.id == id) return R::HistoryResource{capture, 1};
        return {};
    }
    bool save(const std::vector<R::ResourceGrant>& grants) {
        if (!writable) return false;
        QJsonArray rows;
        for (const auto& g : grants) if (g.persistent && context(g.principalId).tokenEpoch == g.tokenEpoch) {
            rows.append(QJsonObject{{"principal", g.principalId}, {"tokenEpoch", QString::number(g.tokenEpoch)},
                {"captureId", g.captureId.text()}, {"generation", QString::number(g.resourceGeneration)},
                {"first", QString::number(g.firstRecordId)}, {"last", QString::number(g.lastRecordId)}});
        }
        return writePrivateJson(path, {{"version", 1}, {"grants", rows}});
    }
    QString pageKey(const QString& client, const SearchHistoryRequest& request) const {
        if (request.cursor.isEmpty()) return {};
        const auto grant = authorization.authorize(context(client), {QUuid(request.capture.str())}, 1);
        if (!grant) return {};
        return client + '/' + QString::number(context(client).tokenEpoch) + '/' + QString::number(grant->accessVersion)
            + '/' + request.capture.str() + '/' + request.cursor + '/' + QString::number(request.rawBytes)
            + '/' + QString::fromLatin1(QCryptographicHash::hash(request.query, QCryptographicHash::Sha256).toHex());
    }
    QJsonObject list(const ListCapturesRequest& request, const QString& client) {
        const auto ctx = context(client);
        auto grants = authorization.grants(ctx);
        QString signature;
        for (const auto& grant : grants) signature += grant.captureId.text() + ':' + QString::number(grant.accessVersion) + ';';
        const auto digest = QString::fromLatin1(QCryptographicHash::hash(signature.toUtf8(), QCryptographicHash::Sha256).toHex());
        QString after;
        if (!request.cursor.isEmpty()) {
            const auto claims = verifyToken(request.cursor, cursorKey);
            if (!claims || claims->value("principal") != client || claims->value("epoch") != QString::number(ctx.tokenEpoch)
                || claims->value("digest") != digest || claims->value("expires").toDouble() <= clock.elapsed())
                return failure("CURSOR_STALE", "Capture list cursor is stale.");
            after = claims->value("after").toString();
        }
        QJsonArray rows;
        bool more = false;
        for (const auto& grant : grants) {
            if (grant.captureId.text() <= after) continue;
            const auto resource = describe(grant.captureId); if (!resource) continue;
            if (rows.size() >= (std::min)(request.limit, 40)) { more = true; break; }
            const auto& capture = resource->capture;
            rows.append(QJsonObject{{"captureId", capture.id.text()}, {"createdAtMs", QString::number(capture.createdMs)},
                {"active", capture.active}, {"highWaterRecordId", QString::number(capture.highWaterRecordId)},
                {"firstRecordId", QString::number(grant.firstRecordId)}, {"lastRecordId", QString::number(grant.lastRecordId)},
                {"sourceCompleteness", completeness(capture.integrity)}, {"persistentGrant", grant.persistent}});
        }
        QString cursor;
        if (more) cursor = signToken({{"principal", client}, {"epoch", QString::number(ctx.tokenEpoch)}, {"digest", digest},
            {"after", rows.last().toObject().value("captureId")}, {"expires", double(clock.elapsed() + 60000)}}, cursorKey);
        return boundedSuccess({{"captures", rows}, {"nextCursor", nullable(cursor)}});
    }
};
HistoryCapabilityAdapter::HistoryCapabilityAdapter(AccessStore& access, QString state, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(this, access, std::move(state))) {}
HistoryCapabilityAdapter::~HistoryCapabilityAdapter() = default;
void HistoryCapabilityAdapter::setSource(R::HistoryService* source) {
    auto& p = *_impl; p.reads.reset(); p.searches.reset(); p.evidence.reset(); p.imports.reset(); p.importedOwners.clear(); p.pages.clear(); p.source = source;
    if (!source) return;
    const QPointer<HistoryCapabilityAdapter> self(this);
    p.port.describe = [self](R::HistoryCaptureId id) -> std::optional<R::HistoryResource> { return self ? self->_impl->describe(id) : std::nullopt; };
    p.port.readCommitted = [weak = QPointer<R::HistoryService>(source)](R::HistoryCaptureId id, R::HistoryRange range, R::HistoryService::ReadDone callback) {
        return weak && weak->readCommitted(id, range, std::move(callback));
    };
    p.reads = std::make_unique<R::HistoryReadService>(p.port, p.authorization);
    p.searches = std::make_unique<R::HistorySearchService>(p.port, p.authorization);
    p.evidence = std::make_unique<E::EvidenceStore>([self](const E::ResourceContext& context, const E::ResourceBinding& binding,
        const QVector<E::RawFragment>& fragments, E::AuthorizationPurpose purpose) {
        if (!self || purpose == E::AuthorizationPurpose::CrossPrincipal || !self->_impl->access.enabled()
            || context.principalId != binding.ownerPrincipalId || self->_impl->context(context.principalId).tokenEpoch != context.tokenGeneration) return false;
        if (binding.kind == E::SourceKind::ImportedFile) {
            const auto owner = self->_impl->importedOwners.find(binding.resourceId);
            return owner != self->_impl->importedOwners.end() && owner->second.principalId == context.principalId
                && owner->second.tokenEpoch == context.tokenGeneration;
        }
        const auto resource = self->_impl->describe({QUuid(binding.resourceId)});
        if (!resource || resource->generation != binding.generation) return false;
        const auto grant = self->_impl->authorization.authorize({context.principalId, context.tokenGeneration}, resource->capture.id, binding.generation);
        if (!grant) return false;
        for (const auto& part : fragments) if (part.recordId < grant->firstRecordId || (grant->lastRecordId && part.recordId > grant->lastRecordId)) return false;
        return true;
    });
    p.evidence->registerSource(E::SourceKind::TerminalCapture, std::make_shared<HistoryEvidenceReader>(p.port,
        [self](const E::ResourceBinding& binding, const R::HistoryRecord& record) {
            if (!self) return false;
            const auto grant = self->_impl->authorization.authorize(self->_impl->context(binding.ownerPrincipalId), record.captureId, binding.generation);
            return grant && R::HistoryAuthorizationStore::allowsRecord(*grant, record);
        }));
    p.imports = std::make_unique<E::ImportedResourceStore>();
    p.evidence->registerSource(E::SourceKind::ImportedFile, p.imports->reader());
}
void HistoryCapabilityAdapter::importFile(QString client, QString path, std::function<void(ImportResult)> callback) {
    const auto ctx = _impl->context(client);
    if (!_impl->imports || !ctx.tokenEpoch) { callback({"Select a registered client before sharing a file."}); return; }
    const QPointer<HistoryCapabilityAdapter> self(this);
    _impl->imports->shareLocalFileAsync({std::move(path)}, client, [self, ctx, callback = std::move(callback)](E::ImportedResource result) {
        if (!self) return;
        if (self->_impl->context(ctx.principalId).tokenEpoch != ctx.tokenEpoch) {
            self->_impl->imports->remove(result.binding.resourceId); callback({"Client identity changed while sharing the file."}); return;
        }
        if (result.error != E::Error::None) { callback({E::errorCode(result.error)}); return; }
        self->_impl->importedOwners[result.binding.resourceId] = ctx;
        callback({{}, result.binding.resourceId, QString::fromLatin1(result.contentHash), result.byteCount});
    });
}
void HistoryCapabilityAdapter::clearImportedFiles(const QString& client) {
    for (auto it = _impl->importedOwners.begin(); it != _impl->importedOwners.end();) {
        if (it->second.principalId == client) { _impl->imports->remove(it->first); it = _impl->importedOwners.erase(it); }
        else ++it;
    }
}
QString HistoryCapabilityAdapter::share(const QString& client, R::HistoryCaptureId id, quint64 first, quint64 last, bool persistent) {
    auto& p = *_impl;
    const auto resource = p.describe(id); const auto ctx = p.context(client);
    if (!p.writable) return "History grants are locked by another process or their private directory is unavailable.";
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
QString HistoryCapabilityAdapter::revoke(const QString& client, R::HistoryCaptureId id) {
    auto& p = *_impl; auto proposed = p.authorization.allGrants();
    proposed.erase(std::remove_if(proposed.begin(), proposed.end(), [&client, id](const auto& value) { return value.principalId == client && value.captureId == id; }), proposed.end());
    if (!p.save(proposed)) return "Could not save the revocation; history permission is still granted.";
    p.authorization.revoke(client, id); p.pages.clear(); return {};
}
QString HistoryCapabilityAdapter::sharingStatus(const QString& client, R::HistoryCaptureId id) const {
    const auto& p = *_impl; const auto grant = p.authorization.authorize(p.context(client), id, 1);
    return grant ? QString("Shared records %1–%2 (%3)").arg(grant->firstRecordId).arg(grant->lastRecordId ? QString::number(grant->lastRecordId) : "committed high water").arg(grant->persistent ? "persistent" : "until exit") : QString("Not shared");
}
Capabilities::ResourceBinding HistoryCapabilityAdapter::binding(const ToolRequest& request) const {
    QString id;
    if (const auto* read = std::get_if<ReadHistoryRequest>(&request)) id = read->capture.str();
    if (const auto* search = std::get_if<SearchHistoryRequest>(&request)) id = search->capture.str();
    if (id.isEmpty()) return Capabilities::DirectoryBinding{};
    return Capabilities::HistoryCaptureBinding{Capabilities::ResourceId{id}, 1};
}
quint64 HistoryCapabilityAdapter::accessVersion(const QString& client, const Capabilities::ResourceBinding& binding) const {
    const auto ctx = _impl->context(client);
    if (const auto* history = std::get_if<Capabilities::HistoryCaptureBinding>(&binding)) {
        const auto grant = _impl->authorization.authorize(ctx, {QUuid(history->id.value())}, history->generation);
        return grant ? grant->accessVersion : 0;
    }
    return ctx.tokenEpoch;
}
std::optional<Capabilities::AuthorizationSnapshot> HistoryCapabilityAdapter::snapshot(const QString& client, const Capabilities::ResourceBinding& binding) const {
    if (!_impl->access.enabled() || !_impl->context(client).tokenEpoch) return {};
    const auto version = accessVersion(client, binding);
    if (const auto* history = std::get_if<Capabilities::HistoryCaptureBinding>(&binding)) {
        if (!version || !_impl->describe({QUuid(history->id.value())})) return {};
    } else if (!std::holds_alternative<Capabilities::DirectoryBinding>(binding)) return {};
    return Capabilities::AuthorizationSnapshot{binding, version};
}
void HistoryCapabilityAdapter::call(const ToolRequest& request, QString client, std::shared_ptr<std::atomic<bool>> cancelled,
                                   std::function<void(QJsonObject)> callback) {
    auto& p = *_impl;
    if (const auto* list = std::get_if<ListCapturesRequest>(&request)) { callback(p.list(*list, client)); return; }
    if (!p.reads || !p.searches) { callback(failure("EVIDENCE_EXPIRED", "History source is unavailable.")); return; }
    const auto ctx = p.context(client);
    const QPointer<HistoryCapabilityAdapter> self(this);
    if (const auto* read = std::get_if<ReadHistoryRequest>(&request)) {
        R::HistoryReadRequest value; value.captureId = {QUuid(read->capture.str())}; value.cursor = read->cursor;
        value.maxResponseBytes = 24000; // Both structuredContent and text carry the result on the wire.
        value.maxPayloadBytes = read->maxBytes;
        p.reads->readAsync(value, ctx, [self, callback = std::move(callback), cancelled](R::HistoryReadPage page) mutable {
            if (!self || cancelled->load()) return;
            if (!page.error.isEmpty()) { callback(failure(page.error, page.error)); return; }
            QJsonArray records;
            for (const auto& fragment : page.records) {
                const auto& r = fragment.record;
                records.append(QJsonObject{{"recordId", QString::number(r.recordId)}, {"lastRecordId", QString::number(r.lastRecordId)},
                    {"byteOffset", double(fragment.byteOffset)}, {"byteCount", double(r.payload.size())},
                    {"base64", QString::fromLatin1(r.payload.toBase64())}, {"kind", r.kind == R::RecordKind::Payload ? "payload" : r.kind == R::RecordKind::Gap ? "gap" : "boundary"},
                    {"reason", r.reason}, {"wallTimestampMs", QString::number(r.wallTimestampMs)},
                    {"lostByteCount", r.lostByteCount ? QJsonValue(QString::number(*r.lostByteCount)) : QJsonValue(QJsonValue::Null)},
                    {"lostRecordCount", r.lostRecordCount ? QJsonValue(QString::number(*r.lostRecordCount)) : QJsonValue(QJsonValue::Null)}});
            }
            callback(boundedSuccess({{"records", records}, {"byteCount", double(page.byteCount)}, {"highWaterRecordId", QString::number(page.highWaterRecordId)},
                {"sourceCompleteness", completeness(page.sourceCompleteness)}, {"truncated", page.truncated}, {"nextCursor", nullable(page.nextCursor)}}));
        }); return;
    }
    const auto* search = std::get_if<SearchHistoryRequest>(&request);
    if (!search) { callback(failure("INVALID_ARGUMENT", "Not a history request.")); return; }
    const QString key = p.pageKey(client, *search);
    for (auto it = p.pages.begin(); it != p.pages.end();) { if (it->second.expires <= p.clock.elapsed()) it = p.pages.erase(it); else ++it; }
    R::HistorySearchRequest value; value.captureId = {QUuid(search->capture.str())}; value.query = search->query;
    value.mode = search->rawBytes ? R::HistorySearchMode::RawBytes : R::HistorySearchMode::Text;
    value.cursor = search->cursor; value.maxMatches = (std::min)(search->maxMatches, 16); value.maxResponseBytes = 16000; value.cancelled = cancelled;
    p.searches->searchAsync(value, ctx, [self, ctx, key, cancelled, callback = std::move(callback), capture = search->capture.str()](R::HistorySearchPage page) mutable {
        if (!self || cancelled->load()) return;
        if (!page.error.isEmpty()) { callback(failure(page.error, page.error)); return; }
        // Evidence publication may be reused only AFTER the search owner has
        // accepted this exact page, query, authorization and session lifetime.
        const auto cached = self->_impl->pages.find(key);
        if (!key.isEmpty() && cached != self->_impl->pages.end()) { callback(cached->second.result); return; }
        auto deliver = [self, key, cancelled, callback = std::move(callback), page](E::EvidenceResult evidence) mutable {
            if (!self || cancelled->load()) return;
            if (!page.hits.empty() && evidence.error != E::Error::None) { callback(failure(E::errorCode(evidence.error), "Could not publish history evidence.")); return; }
            QJsonArray hits, gaps;
            for (const auto& hit : page.hits) {
                QJsonArray ranges; for (const auto& part : hit.ranges) ranges.append(rawRange(part));
                hits.append(QJsonObject{{"ranges", ranges}, {"evidenceId", evidence.reference.evidenceId}});
            }
            for (const auto& part : page.projectionGaps) gaps.append(rawRange(part));
            auto result = boundedSuccess({{"matches", hits}, {"nextCursor", nullable(page.nextCursor)}, {"complete", page.complete},
                {"highWaterRecordId", QString::number(page.highWaterRecordId)}, {"sourceCompleteness", completeness(page.sourceCompleteness)},
                {"projectionVersion", page.projectionVersion}, {"projectionGaps", gaps}, {"reason", nullable(page.reason)},
                {"evidencePersistence", "memory"}, {"evidenceExpiresAt", evidence.reference.expiresAt.toString(Qt::ISODateWithMs)},
                {"evidenceHash", QString::fromLatin1(evidence.reference.contentHash)}});
            if (!key.isEmpty() && self->_impl->pages.size() < 16) self->_impl->pages[key] = {result, self->_impl->clock.elapsed() + 60000};
            callback(std::move(result));
        };
        if (page.hits.empty()) { deliver({}); return; }
        E::EvidenceRequest evidence; evidence.source = {E::SourceKind::TerminalCapture, capture, 1, ctx.principalId};
        for (const auto& hit : page.hits) for (const auto& part : hit.ranges) evidence.fragments.append({part.recordId, quint64(part.byteOffset), quint64(part.byteCount)});
        self->_impl->evidence->createAsync(evidence, {ctx.principalId, ctx.tokenEpoch}, std::move(deliver));
    });
}
}
