#include "HistoryReadService.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QMetaObject>
#include <algorithm>
namespace NovaTerm::Recording {
struct HistoryReadService::Impl {
    struct Position {
        ResourceContext context;
        ResourceGrant grant;
        quint64 highWater{0}, recordId{1};
        qsizetype offset{0};
        qint64 created{0};
    };
    struct Stream {
        QString id, nextCursor, cachedCursor;
        Position position;
        HistoryReadPage cached;
        qsizetype cachedResponseBytes{0}, cachedPayloadBytes{0};
        bool busy{false};
    };
    HistoryReadPort port;
    HistoryAuthorizationStore& authorization;
    QElapsedTimer clock;
    // Each stream retains at most its next position and one exact retry page.
    std::map<QString, std::shared_ptr<Stream>> streams;
    int active{0};
    Impl(HistoryReadPort p, HistoryAuthorizationStore& a) : port(std::move(p)), authorization(a) { clock.start(); }
    bool stale(const Stream& s) const { return clock.elapsed() - s.position.created > 300000; }
    void expire() {
        for (auto i = streams.begin(); i != streams.end();) {
            if (stale(*i->second)) i = streams.erase(i); else ++i;
        }
    }
};
HistoryReadService::HistoryReadService(HistoryReadPort port, HistoryAuthorizationStore& authorization, QObject* p)
    : QObject(p), _impl(std::make_unique<Impl>(std::move(port), authorization)) {}
HistoryReadService::~HistoryReadService() = default;
void HistoryReadService::readAsync(HistoryReadRequest request, ResourceContext context, ReadCallback callback) {
    auto& impl = *_impl;
    const auto fail = [&](const QString& error) { HistoryReadPage p; p.error = error; callback(std::move(p)); };
    if (!request.captureId.valid() || request.maxResponseBytes < 2048 || request.maxResponseBytes > 64 * 1024
        || request.maxPayloadBytes < 1 || request.maxPayloadBytes > 64 * 1024) { fail("INVALID_ARGUMENT"); return; }
    const auto resource = impl.port.describe ? impl.port.describe(request.captureId) : std::nullopt;
    if (!resource) { fail("EVIDENCE_EXPIRED"); return; }
    const auto grant = impl.authorization.authorize(context, request.captureId, resource->generation);
    if (!grant) { fail("PERMISSION_DENIED"); return; }
    if ((request.cancelled && request.cancelled->load()) || (request.admitted && !request.admitted())) { fail("CANCELLED"); return; }
    impl.expire();
    std::shared_ptr<Impl::Stream> stream;
    if (!request.cursor.isEmpty()) {
        for (const auto& entry : impl.streams) {
            const auto& candidate = entry.second;
            if (candidate->nextCursor == request.cursor || candidate->cachedCursor == request.cursor) { stream = candidate; break; }
        }
        if (!stream) { fail("CURSOR_EXPIRED"); return; }
        const auto& position = stream->position;
        if (position.context.principalId != context.principalId || position.context.tokenEpoch != context.tokenEpoch
            || position.grant.captureId != request.captureId || position.grant.resourceGeneration != resource->generation
            || position.grant.accessVersion != grant->accessVersion) { fail("PERMISSION_DENIED"); return; }
        if (stream->busy) { fail("BUSY"); return; }
        if (stream->cachedCursor == request.cursor) {
            if (request.maxPayloadBytes != stream->cachedPayloadBytes || request.maxResponseBytes != stream->cachedResponseBytes) {
                fail("CURSOR_MISMATCH"); return;
            }
            callback(stream->cached); return;
        }
    } else {
        // Bound terminal retry caches as well as unfinished streams. Advancing a stream never consumes capacity.
        if (impl.streams.size() >= 64) { fail("BUSY"); return; }
        stream = std::make_shared<Impl::Stream>(); stream->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto& position = stream->position; position.context = context; position.grant = *grant;
        position.recordId = grant->firstRecordId; position.highWater = resource->capture.highWaterRecordId;
        if (grant->lastRecordId) position.highWater = (std::min)(position.highWater, grant->lastRecordId);
        position.created = impl.clock.elapsed();
    }
    if (impl.active >= 16) { fail("BUSY"); return; }
    if (stream->position.recordId > stream->position.highWater) {
        HistoryReadPage page; page.highWaterRecordId = stream->position.highWater; page.sourceCompleteness = resource->capture.integrity;
        callback(std::move(page)); return;
    }
    if (!impl.port.readCommitted) { fail("SOURCE_UNAVAILABLE"); return; }
    impl.streams[stream->id] = stream; stream->busy = true; ++impl.active;
    const auto position = stream->position;
    const QPointer<HistoryReadService> self(this);
    HistoryRange range; range.firstRecordId = position.recordId; range.lastRecordId = position.highWater;
    range.maxBytes = 256 * 1024; range.maxRecords = 64;
    // Always defer owner-thread completion, including synchronous storage adapters.
    const bool accepted = impl.port.readCommitted(request.captureId, range,
        [self, stream, position, request, callback](ReadResult result) mutable {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, stream, position, request, callback, result = std::move(result)]() mutable {
                if (!self) return;
                const bool admitted = !request.admitted || request.admitted();
                if (!self) return;
                auto& owner = *self->_impl; --owner.active; stream->busy = false;
                HistoryReadPage page; page.highWaterRecordId = position.highWater; page.sourceCompleteness = result.integrity;
                const auto resourceNow = owner.port.describe(request.captureId);
                const auto grantNow = resourceNow ? owner.authorization.authorize(position.context, request.captureId, resourceNow->generation) : std::nullopt;
                if (!resourceNow) page.error = "EVIDENCE_EXPIRED";
                else if (!grantNow || grantNow->accessVersion != position.grant.accessVersion
                         || resourceNow->generation != position.grant.resourceGeneration) page.error = "PERMISSION_DENIED";
                else if (!admitted || (request.cancelled && request.cancelled->load())) page.error = "CANCELLED";
                else if (owner.stale(*stream)) page.error = "CURSOR_EXPIRED";
                else if (result.missing || resourceNow->capture.highWaterRecordId < position.highWater) page.error = "HISTORY_RANGE_MISSING";
                else if (!result.error.isEmpty()) page.error = "SOURCE_UNAVAILABLE";
                size_t inputBytes = 0;
                for (const auto& record : result.records) inputBytes += size_t(record.payload.size());
                if (page.error.isEmpty() && (result.records.size() > 64 || inputBytes > 256 * 1024)) page.error = "SOURCE_LIMIT";
                if (!page.error.isEmpty()) { owner.streams.erase(stream->id); callback(std::move(page)); return; }
                auto next = position;
                qsizetype encodedRemaining = request.maxResponseBytes - 1024;
                for (const auto& record : result.records) {
                    const auto last = record.lastRecordId ? record.lastRecordId : record.recordId;
                    if (record.recordId > next.recordId || last < next.recordId || last > position.highWater) {
                        page.error = "HISTORY_RANGE_MISSING"; break;
                    }
                    if (!HistoryAuthorizationStore::allowsRecord(position.grant, record)) {
                        // A coalesced gap can straddle the grant's lower bound. Its omitted
                        // authorized records must remain visible as missing, without disclosing
                        // metadata or loss counts belonging to the unshared part.
                        if (record.kind == RecordKind::Gap && last >= position.grant.firstRecordId
                            && (!position.grant.lastRecordId || record.recordId <= position.grant.lastRecordId)) {
                            page.error = "HISTORY_RANGE_MISSING"; break;
                        }
                        next.recordId = last + 1; next.offset = 0; continue;
                    }
                    constexpr qsizetype fragmentMetadata = 512;
                    if (encodedRemaining < fragmentMetadata + 4) break;
                    const auto available = (std::min)(((encodedRemaining - fragmentMetadata) / 4) * 3,
                        request.maxPayloadBytes - page.byteCount);
                    const auto remaining = record.payload.size() - next.offset;
                    if (!available && remaining > 0) break;
                    if (remaining < 0) { page.error = "HISTORY_RANGE_MISSING"; break; }
                    const auto count = (std::min)(available, remaining);
                    HistoryReadFragment fragment{record, next.offset};
                    fragment.record.reason = fragment.record.reason.left(128);
                    fragment.record.payload = record.payload.mid(next.offset, count);
                    page.records.push_back(std::move(fragment)); page.byteCount += count;
                    encodedRemaining -= fragmentMetadata + ((count + 2) / 3) * 4;
                    next.offset += count;
                    if (next.offset < record.payload.size()) break;
                    next.recordId = last + 1; next.offset = 0;
                }
                if (page.error.isEmpty() && next.recordId <= position.highWater) {
                    if (next.recordId == position.recordId && next.offset == position.offset) page.error = "HISTORY_RANGE_MISSING";
                    else { page.truncated = true; page.nextCursor = QUuid::createUuid().toString(QUuid::WithoutBraces); }
                }
                if (!page.error.isEmpty()) {
                    page.records.clear(); page.byteCount = 0; owner.streams.erase(stream->id);
                } else {
                    stream->position = next; stream->nextCursor = page.nextCursor;
                    stream->cachedCursor = request.cursor; stream->cachedResponseBytes = request.maxResponseBytes;
                    stream->cachedPayloadBytes = request.maxPayloadBytes; stream->cached = page;
                    if (request.cursor.isEmpty() && page.nextCursor.isEmpty()) owner.streams.erase(stream->id);
                }
                callback(std::move(page));
            }, Qt::QueuedConnection);
        });
    if (!accepted) { --impl.active; stream->busy = false; impl.streams.erase(stream->id); fail("BUSY"); }
}
}
