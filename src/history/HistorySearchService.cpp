#include "HistorySearchService.h"
#include "HistoryProjectionDecoder.h"
#include <QPointer>
#include <QRunnable>
#include <algorithm>
#include <mutex>
namespace NovaTerm::Recording {
using Clock=std::chrono::steady_clock;
struct HistorySearchService::State {
    HistorySearchRequest request;
    ResourceContext context;
    ResourceGrant grant;
    CaptureDescriptor descriptor;
    quint64 highWater{0}, record{1}, scanned{0};
    quint32 offset{0};
    Clock::time_point created,lastUsed,deadline;
    bool busy{false}, eof{false}, finished{false}, needsRead{false};
    QString nextToken, cachedToken;
    HistorySearchPage page,cached;
    std::optional<HistoryRecord> data;
    std::optional<HistoryMatch> pendingMatch;
    HistoryProjectionDecoder decoder;
    HistoryLiteralMatcher matcher;
    std::deque<std::optional<HistoryProjectionDecoder::Unit>> projected;
    Cancellation cancelled;
    std::function<void(HistorySearchPage)> done;
};
struct HistorySearchService::Mailbox { std::mutex mutex; HistorySearchService* owner; };
HistorySearchService::HistorySearchService(HistoryReadService& r,QObject* p):HistorySearchService(r,Limits{},p) {}
HistorySearchService::HistorySearchService(HistoryReadService& r,Limits l,QObject* p):QObject(p),_reads(r),_limits(l),_mailbox(std::make_shared<Mailbox>()) {
    _mailbox->owner=this; _pool.setMaxThreadCount(2); _pool.setExpiryTimeout(1000);
    _sweep.setInterval(1000); connect(&_sweep,&QTimer::timeout,this,[this]{sweep();});_sweep.start();
}
HistorySearchService::~HistorySearchService() {
    {std::lock_guard<std::mutex> lock(_mailbox->mutex);_mailbox->owner=nullptr;}
    for(const auto& s:_states) if(s->cancelled) s->cancelled->store(true);
    _pool.clear(); _pool.waitForDone(); // only a bounded CPU batch; no disk/GUI dependency
}
void HistorySearchService::sweep() {
    const auto now=Clock::now();
    for(auto it=_states.begin();it!=_states.end();) {
        const auto& s=*it;
        if(now-s->created>=_limits.absoluteTtl || now-s->lastUsed>=_limits.idleTtl) {
            if(s->busy) {s->cancelled->store(true);++it;} else it=_states.erase(it);
        } else ++it;
    }
}
void HistorySearchService::searchAsync(HistorySearchRequest r,ResourceContext c,std::function<void(HistorySearchPage)> done,Cancellation cancelled) {
    const auto fail=[&](const char* e){HistorySearchPage p;p.error=QString::fromLatin1(e);done(std::move(p));};
    const auto grant=_reads.authorize(c,r.capture); if(!grant){fail("PERMISSION_DENIED");return;}
    const auto descriptor=_reads.descriptor(r.capture);if(!descriptor){fail("EVIDENCE_EXPIRED");return;}
    if(r.query.isEmpty() || r.query.size()>4096 || r.maxMatches<1 || r.maxMatches>128){fail("INVALID_ARGUMENT");return;}
    if(r.mode==SearchMode::Text && QString::fromUtf8(r.query).toUtf8()!=r.query){fail("INVALID_ARGUMENT");return;}
    sweep(); std::shared_ptr<State> state;
    if(!r.cursor.isEmpty()) {
        for(const auto& s:_states) if(s->nextToken==r.cursor || s->cachedToken==r.cursor) {state=s;break;}
        if(!state){fail("SEARCH_CURSOR_EXPIRED");return;}
        if(state->context.principal!=c.principal || state->context.credential!=c.credential || state->grant.version!=grant->version){fail("PERMISSION_DENIED");return;}
        if(state->request.capture!=r.capture || state->request.query!=r.query || state->request.mode!=r.mode || state->request.maxMatches!=r.maxMatches){fail("CURSOR_MISMATCH");return;}
        if(state->busy){fail("BUSY");return;}
        if(state->cachedToken==r.cursor){done(state->cached);return;}
    }
    size_t active=0,ownActive=0,ownStates=0;
    for(const auto& s:_states){if(s->busy){++active;if(s->context.principal==c.principal)++ownActive;}if(s->context.principal==c.principal)++ownStates;}
    if(active>=2 || ownActive || (!state && (_states.size()>=_limits.states || ownStates>=_limits.principalStates))){fail("BUSY");return;}
    if(!state) {
        state=std::make_shared<State>();state->context=c;state->grant=*grant;state->descriptor=*descriptor;
        state->highWater=grant->last?(std::min)(grant->last,descriptor->highWaterRecordId):descriptor->highWaterRecordId;
        state->record=grant->first;state->created=Clock::now();state->matcher=HistoryLiteralMatcher(r.query);_states.push_back(state);
    }
    state->request=std::move(r);state->page={};state->page.highWater=state->highWater;state->page.integrity=state->descriptor.integrity;
    state->lastUsed=Clock::now();state->deadline=(std::min)(state->lastUsed+_limits.pageTime,state->created+_limits.absoluteTtl);
    state->cancelled=cancelled?std::move(cancelled):std::make_shared<std::atomic<bool>>(false);
    state->done=std::move(done);state->busy=true;state->scanned=0;step(state);
}
void HistorySearchService::step(const std::shared_ptr<State>& s) {
    const auto grant=_reads.authorize(s->context,s->request.capture);
    if(!grant || grant->version!=s->grant.version){finish(s,"PERMISSION_DENIED");return;}
    if(!_reads.descriptor(s->request.capture)){finish(s,"EVIDENCE_EXPIRED");return;}
    if(s->cancelled->load()){finish(s,"CANCELLED");return;}
    if(s->data || !s->projected.empty() || s->record>s->highWater) {compute(s);return;}
    if(Clock::now()>=s->deadline || s->scanned>=_limits.scanBytes){finish(s);return;}
    QPointer<HistorySearchService> guard(this);
    const auto complete=[this,guard,s](ReadResult r){
        if(!guard) return;
        if(r.missing || !r.error.isEmpty() || r.records.empty() || r.records.front().recordId>s->record
            || r.records.front().lastRecordId<s->record){finish(s,"EVIDENCE_EXPIRED");return;}
        s->data=std::move(r.records.front());s->offset=0;compute(s);
    };
    if(!_reads.source().read(s->request.capture,{s->record,s->highWater,65536,1},complete)) finish(s,"BUSY");
}
void HistorySearchService::compute(const std::shared_ptr<State>& s) {
    const auto mailbox=_mailbox;const auto limits=_limits;
    _pool.start(QRunnable::create([s,mailbox,limits] {
        const auto queue=[&](HistoryProjectionDecoder::Unit u){s->projected.push_back(std::move(u));};
        size_t spanCount=0;
        for(const auto& match:s->page.matches)spanCount+=match.spans.size();
        if(s->pendingMatch){spanCount+=s->pendingMatch->spans.size();s->page.matches.push_back(std::move(*s->pendingMatch));s->pendingMatch.reset();}
        const auto accept=[&](HistoryMatch hit){
            if(hit.spans.size()>128){s->page.reason="MATCH_RANGE_LIMIT";s->page.projectionGap=std::move(hit.spans);s->finished=true;return;}
            if(spanCount+hit.spans.size()>128){s->pendingMatch=std::move(hit);return;}
            spanCount+=hit.spans.size();s->page.matches.push_back(std::move(hit));
        };
        const auto drain=[&] {
            while(!s->projected.empty() && !s->pendingMatch && !s->finished && s->page.matches.size()<size_t(s->request.maxMatches)) {
                auto u=std::move(s->projected.front());s->projected.pop_front();
                if(!u){s->matcher.reset();continue;}
                // Valid UTF-8 matches cover complete codepoints. Replacement bytes
                // all map to the same original invalid sequence.
                std::vector<RawSpan> origins;for(auto span:u->spans)for(quint32 j=0;j<span.length;++j)origins.push_back({span.record,span.offset+j,1});
                for(qsizetype i=0;i<u->bytes.size();++i) {
                    const auto origin=origins[(std::min)(size_t(i),origins.size()-1)];
                    if(auto hit=s->matcher.feed(u->bytes[i],origin)) accept(std::move(*hit));
                }
            }
        };
        s->needsRead=false;drain();
        while(!s->pendingMatch && !s->finished && s->page.matches.size()<size_t(s->request.maxMatches) && !s->cancelled->load()) {
            if(Clock::now()>=s->deadline || s->scanned>=limits.scanBytes)break;
            if(!s->data) {
                if(s->record<=s->highWater){s->needsRead=true;break;}
                if(!s->eof){if(s->request.mode==SearchMode::Text)s->decoder.flush(queue);s->eof=true;drain();}
                if(s->projected.empty()&&!s->pendingMatch&&!s->finished)s->page.complete=true;break;
            }
            const auto& r=*s->data;
            const bool inTime=r.wallTimestampMs>=s->grant.fromMs && (!s->grant.toMs || r.wallTimestampMs<=s->grant.toMs);
            if(r.kind!=RecordKind::Payload || !inTime) {
                if(s->request.mode==SearchMode::Text){s->decoder.flush(queue);s->decoder.reset();}
                s->projected.push_back(std::nullopt);s->record=r.lastRecordId+1;s->data.reset();drain();continue;
            }
            if(s->offset>=quint32(r.payload.size())){s->record=r.lastRecordId+1;s->data.reset();continue;}
            const auto origin=RawSpan{r.recordId,s->offset,1};const auto c=r.payload[s->offset++];++s->scanned;
            if(s->request.mode==SearchMode::RawBytes){if(auto hit=s->matcher.feed(c,origin))accept(std::move(*hit));}
            else if(!s->decoder.feed(c,origin,queue)) {s->page.reason="PROJECTION_LIMIT";s->page.projectionGap=s->decoder.limitRange();s->finished=true;break;}
            drain();
        }
        // Serialize by QObject context. The mailbox closes before destruction;
        // workers never hold raw service/Session/source pointers.
        std::lock_guard<std::mutex> lock(mailbox->mutex);
        if(auto* owner=mailbox->owner) QMetaObject::invokeMethod(owner,[owner,s]{
            if(s->needsRead)owner->step(s);else owner->finish(s);
        },Qt::QueuedConnection);
    }));
}
void HistorySearchService::finish(const std::shared_ptr<State>& s,QString error) {
    const auto grant=_reads.authorize(s->context,s->request.capture);
    if(!grant || grant->version!=s->grant.version)error="PERMISSION_DENIED";
    else if(!_reads.descriptor(s->request.capture))error="EVIDENCE_EXPIRED";
    else if(s->cancelled->load())error="CANCELLED";
    if(!error.isEmpty()){s->page={};s->page.error=std::move(error);s->finished=true;}
    if(s->page.complete)s->finished=true;
    if(!s->finished)s->page.cursor=QUuid::createUuid().toString(QUuid::WithoutBraces);
    s->cachedToken=s->request.cursor;s->nextToken=s->page.cursor;s->cached=s->page;s->lastUsed=Clock::now();
    s->busy=false;auto done=std::move(s->done);s->cancelled.reset();
    if(s->finished){s->data.reset();s->decoder.reset();s->projected.clear();s->matcher=HistoryLiteralMatcher();}
    // No request cursor exists for the initial page; a terminal initial page
    // has no reachable retry handle and must not occupy a cursor lease.
    if(!s->page.error.isEmpty() || (s->finished&&s->cachedToken.isEmpty()))
        _states.erase(std::remove(_states.begin(),_states.end(),s),_states.end());
    done(s->page);
}
}
