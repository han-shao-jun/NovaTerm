#include "HistoryReadService.h"
#include <QPointer>
#include <algorithm>
namespace NovaTerm::Recording {
HistoryReadService::HistoryReadService(HistorySourceReader& source, HistoryAuthorizationStore& auth,
    ValidateContext validate, QObject* parent) : QObject(parent),_source(source),_authorization(auth),_validate(std::move(validate)) {}
std::optional<ResourceGrant> HistoryReadService::authorize(const ResourceContext& c,HistoryCaptureId id) const {
    return _validate && _validate(c) ? _authorization.authorize(c,id) : std::nullopt;
}
std::optional<CaptureDescriptor> HistoryReadService::descriptor(HistoryCaptureId id) const {
    for (const auto& d:_source.captures()) if (d.id==id) return d;
    return std::nullopt;
}
std::vector<CaptureDescriptor> HistoryReadService::list(const ResourceContext& c) const {
    std::vector<CaptureDescriptor> result;
    for (auto d:_source.captures()) if (const auto g=authorize(c,d.id)) {
        d.highWaterRecordId=g->last ? (std::min)(g->last,d.highWaterRecordId) : d.highWaterRecordId;
        // Source metadata must not disclose unshared byte totals or source Session identity.
        d.bytes=0; d.gapBytes=0; d.sessionId={}; result.push_back(d);
    }
    return result;
}
void HistoryReadService::readAsync(HistoryReadRequest request,ResourceContext context,
    std::function<void(HistoryReadPage)> done,Cancellation cancelled) {
    const auto fail=[&](const char* e) { HistoryReadPage p; p.error=QString::fromLatin1(e); done(std::move(p)); };
    const auto grant=authorize(context,request.capture);
    if (!grant) { fail("PERMISSION_DENIED"); return; }
    const auto d=descriptor(request.capture); if (!d) { fail("EVIDENCE_EXPIRED"); return; }
    const auto now=std::chrono::steady_clock::now();
    for (auto it=_cursors.begin();it!=_cursors.end();) if (now>=it->second.expires) it=_cursors.erase(it); else ++it;
    Cursor state{context,request.capture,grant->version,
        grant->last ? (std::min)(grant->last,d->highWaterRecordId) : d->highWaterRecordId,grant->first,0,now+std::chrono::minutes(5)};
    if (!request.cursor.isEmpty()) {
        const auto it=_cursors.find(request.cursor);
        if (it==_cursors.end()) { fail("HISTORY_CURSOR_EXPIRED"); return; }
        state=it->second;
        if (state.context.principal!=context.principal || state.context.credential!=context.credential
            || state.capture!=request.capture || state.version!=grant->version) { fail("PERMISSION_DENIED"); return; }
    }
    if (_active>=8 || (request.cursor.isEmpty()&&_cursors.size()+_active>=MaxCursors)) { fail("BUSY"); return; }
    if (cancelled && cancelled->load()) { fail("CANCELLED"); return; }
    if (state.record>state.highWater) { HistoryReadPage p; p.highWater=state.highWater; p.integrity=d->integrity; done(std::move(p)); return; }
    const auto guard=QPointer<HistoryReadService>(this); ++_active;
    // Fetch one full P2 record then split at byte boundaries; binary is encoded by the adapter.
    HistoryRange range{state.record,state.highWater,64*1024,1};
    const auto completion=[this,guard,state,grant=*grant,request,context,done,cancelled](ReadResult r) mutable {
        if (!guard) return; --_active; HistoryReadPage page;
        const auto current=authorize(context,request.capture);
        if (!current || current->version!=state.version) page.error="PERMISSION_DENIED";
        else if (cancelled && cancelled->load()) page.error="CANCELLED";
        else if (!descriptor(request.capture) || r.missing || !r.error.isEmpty() || r.records.empty()) page.error="EVIDENCE_EXPIRED";
        if (!page.error.isEmpty()) { done(std::move(page)); return; }
        auto next=state; page.highWater=state.highWater; page.integrity=r.integrity;
        auto record=std::move(r.records.front());
        const auto sourceLength=quint32(record.payload.size());
        const auto sourceLast=record.lastRecordId;
        if (record.recordId>state.record || record.lastRecordId<state.record || state.offset>quint32(record.payload.size())) {
            page.error="EVIDENCE_EXPIRED"; done(std::move(page)); return;
        }
        const bool inTime=record.wallTimestampMs>=grant.fromMs && (!grant.toMs || record.wallTimestampMs<=grant.toMs);
        if (inTime) {
            const auto count=(std::min)(qsizetype(std::clamp(request.maxBytes,1,16*1024)),record.payload.size()-state.offset);
            record.payload=record.payload.mid(state.offset,count); page.byteCount=quint64(count);
            if (record.kind==RecordKind::Gap) { record.recordId=(std::max)(record.recordId,grant.first);
                record.lastRecordId=(std::min)(record.lastRecordId,state.highWater); record.lostByteCount.reset(); record.lostRecordCount.reset(); }
            page.records.push_back({std::move(record),state.offset});
            next.offset+=quint32(count);
        }
        if (!inTime || next.offset>=sourceLength) { next.record=sourceLast+1; next.offset=0; }
        if (next.record<=state.highWater) {
            page.cursor=QUuid::createUuid().toString(QUuid::WithoutBraces);
            _cursors.emplace(page.cursor,std::move(next)); page.truncated=true;
        }
        if (!request.cursor.isEmpty()) _cursors.erase(request.cursor);
        done(std::move(page));
    };
    if (!_source.read(request.capture,range,completion)) { --_active; fail("BUSY"); }
}
}
