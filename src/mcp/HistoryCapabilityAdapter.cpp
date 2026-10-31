#include "HistoryCapabilityAdapter.h"
#include "CapabilityAdmission.h"
#include "protocol/ToolCodec.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "recording/HistoryService.h"
#include "resources/RecordingHistoryReadAdapter.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include "ResourceComposition.h"
#include "resources/HistoryCapabilities.h"
#include <QPointer>
#include <algorithm>
#include <type_traits>

namespace NovaTerm::Mcp {
namespace R = Recording;
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
}
struct HistoryCapabilityAdapter::Impl {
    AccessStore& access;
    std::shared_ptr<Resources::ResourceSharingService> resources;
    std::unique_ptr<Resources::HistoryCapabilities> ports;
    QByteArray cursorKey{randomBytes()};
    QElapsedTimer clock;
    Impl(AccessStore& access, std::shared_ptr<Resources::ResourceSharingService> resources)
        : access(access), resources(std::move(resources)) { clock.start(); }
};
HistoryCapabilityAdapter::HistoryCapabilityAdapter(AccessStore& access, QString state, QObject* parent)
    : HistoryCapabilityAdapter(access, makeResourceSharingService(access, state), parent) {}
HistoryCapabilityAdapter::HistoryCapabilityAdapter(AccessStore& access, std::shared_ptr<Resources::ResourceSharingService> resources, QObject* parent)
    : QObject(parent), _impl(std::make_unique<Impl>(access, std::move(resources))) {}
HistoryCapabilityAdapter::~HistoryCapabilityAdapter() = default;
void HistoryCapabilityAdapter::bindDispatcher(Capabilities::CapabilityDispatcher& dispatcher) {
    _impl->ports = std::make_unique<Resources::HistoryCapabilities>(dispatcher, *_impl->resources);
}
std::shared_ptr<Resources::ResourceSharingService> HistoryCapabilityAdapter::resources() const { return _impl->resources; }
void HistoryCapabilityAdapter::setSource(R::HistoryService* source) {
    _impl->resources->setHistorySource(R::recordingReadPort(source));
}
void HistoryCapabilityAdapter::importFile(QString client, QString path, std::function<void(ImportResult)> done) {
    _impl->resources->importFile(std::move(client), std::move(path), std::move(done));
}
void HistoryCapabilityAdapter::clearImportedFiles(const QString& client) { _impl->resources->clearImportedFiles(client); }
Capabilities::ResourceBinding HistoryCapabilityAdapter::binding(const HistoryToolRequest& request) const {
    return std::visit([this](const auto& value) -> Capabilities::ResourceBinding {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ListCapturesRequest>) return Capabilities::DirectoryBinding{};
        else {
            const auto resource = _impl->resources->describe({QUuid(value.capture.str())});
            return Capabilities::HistoryCaptureBinding{Capabilities::ResourceId{value.capture.str()}, resource ? resource->generation : 0};
        }
    }, request);
}
quint64 HistoryCapabilityAdapter::accessVersion(const QString& client, const Capabilities::ResourceBinding& binding) const {
    const auto context = _impl->resources->context(client);
    if (const auto* capture = std::get_if<Capabilities::HistoryCaptureBinding>(&binding)) {
        const auto grant = _impl->resources->authorize(context, {QUuid(capture->id.value())}, capture->generation);
        return grant ? grant->accessVersion : 0;
    }
    return context.tokenEpoch;
}
std::optional<Capabilities::AuthorizationSnapshot> HistoryCapabilityAdapter::snapshot(const QString& client, const Capabilities::ResourceBinding& binding) const {
    if (!_impl->access.enabled() || !_impl->resources->context(client).tokenEpoch) return {};
    const auto version = accessVersion(client, binding);
    if (const auto* capture = std::get_if<Capabilities::HistoryCaptureBinding>(&binding)) {
        const auto resource = _impl->resources->describe({QUuid(capture->id.value())});
        if (!version || !resource || resource->generation != capture->generation) return {};
    } else if (!std::holds_alternative<Capabilities::DirectoryBinding>(binding)) return {};
    return Capabilities::AuthorizationSnapshot{binding, version};
}
namespace {
QJsonObject rejected(Capabilities::Rejection rejection) {
    return ToolCodec::encodeFailure(capabilityError(rejection));
}
}
void HistoryCapabilityAdapter::call(const HistoryToolRequest& request, const Capabilities::InvocationContext& invocation,
    std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(QJsonObject)> done) {
    if (!_impl->ports) { done(failure("APP_UNAVAILABLE", "History domain is not connected.")); return; }
    const auto context = _impl->resources->context(invocation.principal.value());
    const QPointer<HistoryCapabilityAdapter> self(this);
    if (const auto* list = std::get_if<ListCapturesRequest>(&request)) {
        Resources::CaptureListQuery query; query.limit = (std::min)(list->limit, 40);
        if (!list->cursor.isEmpty()) {
            const auto claims = verifyToken(list->cursor, _impl->cursorKey);
            if (!claims || claims->value("principal") != context.principalId || claims->value("epoch") != QString::number(context.tokenEpoch)
                || claims->value("expires").toDouble() <= _impl->clock.elapsed()) { done(failure("CURSOR_STALE", "Capture list cursor is stale.")); return; }
            query.after = claims->value("after").toString(); query.digest = claims->value("digest").toString();
        }
        _impl->ports->list.invoke(query, invocation, [self, context, done = std::move(done)](auto result) mutable {
            if (!self) return;
            if (const auto* rejection = std::get_if<Capabilities::Rejection>(&result)) { done(rejected(*rejection)); return; }
            const auto& page = std::get<Resources::CaptureListPage>(result);
            if (!page.error.isEmpty()) { done(failure(page.error, page.error)); return; }
            QJsonArray rows;
            for (const auto& row : page.captures) rows.append(QJsonObject{{"captureId", row.id.text()}, {"createdAtMs", QString::number(row.createdMs)},
                {"active", row.active}, {"highWaterRecordId", QString::number(row.highWater)},
                {"sourceCompleteness", completeness(row.integrity)}});
            QString cursor;
            if (!page.nextAfter.isEmpty()) cursor = signToken({{"principal", context.principalId}, {"epoch", QString::number(context.tokenEpoch)},
                {"digest", page.digest}, {"after", page.nextAfter}, {"expires", double(self->_impl->clock.elapsed() + 60000)}}, self->_impl->cursorKey);
            done(boundedSuccess({{"captures", rows}, {"nextCursor", nullable(cursor)}}));
        }); return;
    }
    if (const auto* read = std::get_if<ReadHistoryRequest>(&request)) {
        R::HistoryReadRequest query; query.captureId = {QUuid(read->capture.str())}; query.cursor = read->cursor;
        query.maxResponseBytes = 24000; query.maxPayloadBytes = read->maxBytes; query.cancelled = std::move(cancelled);
        _impl->ports->read.invoke(query, invocation, [self, done = std::move(done)](auto result) mutable {
            if (!self) return;
            if (const auto* rejection = std::get_if<Capabilities::Rejection>(&result)) { done(rejected(*rejection)); return; }
            const auto& page = std::get<R::HistoryReadPage>(result);
            if (!page.error.isEmpty()) { done(failure(page.error, page.error)); return; }
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
            done(boundedSuccess({{"records", records}, {"byteCount", double(page.byteCount)}, {"highWaterRecordId", QString::number(page.highWaterRecordId)},
                {"sourceCompleteness", completeness(page.sourceCompleteness)}, {"truncated", page.truncated}, {"nextCursor", nullable(page.nextCursor)}}));

        }); return;
    }
    const auto& search = std::get<SearchHistoryRequest>(request);
    R::HistorySearchRequest query; query.captureId = {QUuid(search.capture.str())}; query.cursor = search.cursor; query.query = search.query;
    query.mode = search.rawBytes ? R::HistorySearchMode::RawBytes : R::HistorySearchMode::Text;
    query.maxMatches = (std::min)(search.maxMatches, 16); query.maxResponseBytes = 16000; query.cancelled = std::move(cancelled);
    _impl->ports->search.invoke(query, invocation, [self, done = std::move(done)](auto result) mutable {
        if (!self) return;
        if (const auto* rejection = std::get_if<Capabilities::Rejection>(&result)) { done(rejected(*rejection)); return; }
        const auto& value = std::get<Resources::EvidenceSearchPage>(result); const auto& page = value.page; const auto& evidence = value.evidence;
        if (!page.error.isEmpty()) { done(failure(page.error, page.error)); return; }
            QJsonArray hits, gaps;
            for (const auto& hit : page.hits) {
                QJsonArray ranges; for (const auto& part : hit.ranges) ranges.append(rawRange(part));
                hits.append(QJsonObject{{"ranges", ranges}, {"evidenceId", evidence.evidenceId}});
            }
            for (const auto& part : page.projectionGaps) gaps.append(rawRange(part));
            auto payload = boundedSuccess({{"matches", hits}, {"nextCursor", nullable(page.nextCursor)}, {"complete", page.complete},
                {"highWaterRecordId", QString::number(page.highWaterRecordId)}, {"sourceCompleteness", completeness(page.sourceCompleteness)},
                {"projectionVersion", page.projectionVersion}, {"projectionGaps", gaps}, {"reason", nullable(page.reason)},
                {"evidencePersistence", "memory"}, {"evidenceExpiresAt", evidence.expiresAt.toString(Qt::ISODateWithMs)},
                {"evidenceHash", QString::fromLatin1(evidence.contentHash)}});
            done(std::move(payload));

    });
}
}
