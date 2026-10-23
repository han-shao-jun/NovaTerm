#include "HistoryCapabilityAdapter.h"
#include "McpAccess.h"
#include "McpProtocol.h"
#include "history/HistoryService.h"
#include "evidence/EvidenceStore.h"
#include "evidence/ImportedResourceStore.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <algorithm>
namespace NovaTerm::Mcp {
using namespace Recording;
namespace {
QString completeness(Integrity i){switch(i){case Integrity::Lossless:return "lossless";case Integrity::KnownGap:return "known_gap";case Integrity::UnknownTail:return "unknown_tail";case Integrity::RecoveredUnsealed:return "recovered_unsealed";}return "unknown_tail";}
QJsonArray ranges(const std::vector<RawSpan>& spans){QJsonArray a;for(auto s:spans)a.append(QJsonObject{{"recordId",QString::number(s.record)},{"byteOffset",int(s.offset)},{"byteCount",int(s.length)}});return a;}
QJsonValue gapRange(const std::vector<RawSpan>& spans){
    if(spans.empty())return QJsonValue::Null;
    auto first=spans.front(),last=spans.front();
    for(const auto span:spans){
        if(span.record<first.record||(span.record==first.record&&span.offset<first.offset))first=span;
        if(span.record>last.record||(span.record==last.record&&span.offset+span.length>last.offset+last.length))last=span;
    }
    return QJsonObject{{"start",QJsonObject{{"recordId",QString::number(first.record)},{"byteOffset",int(first.offset)}}},
        {"endExclusive",QJsonObject{{"recordId",QString::number(last.record)},{"byteOffset",int(last.offset+last.length)}}}};
}
class SourceAdapter final:public HistorySourceReader {
public:
    explicit SourceAdapter(HistoryService* s):source(s){}
    QPointer<HistoryService> source;
    std::vector<CaptureDescriptor> captures()const override{return source?source->captures():std::vector<CaptureDescriptor>{};}
    bool read(HistoryCaptureId id,HistoryRange range,std::function<void(ReadResult)> done)override{return source&&source->readCommitted(id,range,std::move(done));}
};
class CaptureEvidence final:public Evidence::EvidenceSourceReader {
public:
    explicit CaptureEvidence(HistoryReadService& service):reads(&service){}
    QPointer<HistoryReadService> reads;
    bool available(const Evidence::Source& s)const override{return reads&&reads->descriptor({QUuid(s.resource)}).has_value();}
    bool authorized(const Evidence::Source& s,const Evidence::Context& c)const override{
        if(!reads || s.kind!=Evidence::SourceKind::TerminalCapture || s.generation!=1 || s.ranges.empty())return false;
        const auto g=reads->authorize({c.principal,c.credential},{QUuid(s.resource)});if(!g)return false;
        for(auto r:s.ranges)if(r.record<g->first || (g->last&&r.record>g->last))return false;
        return true;
    }
    void read(const Evidence::Source& s,const Evidence::Context& c,Evidence::Done done)override{
        if(!authorized(s,c)){done({{},"PERMISSION_DENIED",{}, {}});return;}
        struct Job{Evidence::Source source;Evidence::Context context;Evidence::Done done;size_t index{0};QByteArray bytes;};
        auto job=std::make_shared<Job>();job->source=s;job->context=c;job->done=std::move(done);next(job);
    }
private:
    template<class Job> void next(const std::shared_ptr<Job>& job){
        if(!authorized(job->source,job->context)){job->done({{},"PERMISSION_DENIED",{}, {}});return;}
        const auto r=job->source.ranges[job->index];const auto id=HistoryCaptureId{QUuid(job->source.resource)};
        QPointer<HistoryReadService> guard(reads);
        const auto complete=[this,guard,job,r,id](ReadResult result){
            if(!guard){job->done({{},"EVIDENCE_EXPIRED",{}, {}});return;}
            const auto g=reads->authorize({job->context.principal,job->context.credential},id);
            if(!g){job->done({{},"PERMISSION_DENIED",{}, {}});return;}
            if(result.missing || !result.error.isEmpty() || result.records.empty()){job->done({{},"EVIDENCE_EXPIRED",{}, {}});return;}
            const auto& record=result.records.front();
            if(record.recordId!=r.record || record.kind!=RecordKind::Payload || quint64(r.offset)+r.length>quint64(record.payload.size())
                || record.wallTimestampMs<g->fromMs || (g->toMs&&record.wallTimestampMs>g->toMs)) {job->done({{},"PERMISSION_DENIED",{}, {}});return;}
            job->bytes+=record.payload.mid(r.offset,r.length);
            if(++job->index<job->source.ranges.size())next(job);
            else job->done({std::move(job->bytes),{},completeness(result.integrity),{{"source","receive_stream"}}});
        };
        if(!reads->source().read(id,{r.record,r.record,65536,1},complete))job->done({{},"BUSY",{}, {}});
    }
};
}
void registerHistoryCapabilities(Capabilities::CapabilityRegistry& registry){
    using namespace Capabilities;
    registry.add({CapabilityId{"history.capture.list"},1,ImplementationStatus::Implemented,ResourceKind::Directory,Policy::DirectoryRead,"novaterm_list_captures"});
    registry.add({CapabilityId{"history.read"},1,ImplementationStatus::Implemented,ResourceKind::HistoryCapture,Policy::ReadOutput,"novaterm_read_history"});
    registry.add({CapabilityId{"history.search"},1,ImplementationStatus::Implemented,ResourceKind::HistoryCapture,Policy::ReadOutput,"novaterm_search_history"});
}
struct HistoryCapabilityAdapter::Impl {
    AccessStore& access;Capabilities::CapabilityDispatcher& dispatcher;QString stateFile;HistoryCapabilityAdapter* owner;
    SourceAdapter source;HistoryAuthorizationStore auth;HistoryReadService reads;HistorySearchService search;Evidence::EvidenceStore evidence;
    struct Operation {QString peer,id;ResourceContext context;HistoryToolRequest request;Cancellation cancelled;Capabilities::InvocationLease lease;std::function<void(QJsonObject)> done;};
    std::map<QString,std::shared_ptr<Operation>> operations;
    struct ListCursor {ResourceContext context;QString after;quint64 version;std::chrono::steady_clock::time_point expires;};
    std::map<QString,ListCursor> listCursors;
    QTimer timer;
    Impl(HistoryCapabilityAdapter* q,AccessStore& a,HistoryService* s,Capabilities::CapabilityDispatcher& d,QString f)
        :access(a),dispatcher(d),stateFile(std::move(f)),owner(q),source(s),reads(source,auth,[this](const ResourceContext& c){return valid(c);}),search(reads){
        evidence.registerSource(Evidence::SourceKind::TerminalCapture,std::make_shared<CaptureEvidence>(reads));
        auto memory=std::make_shared<Evidence::ImportedResourceStore>();
        evidence.registerSource(Evidence::SourceKind::DiagnosticResult,memory);evidence.registerSource(Evidence::SourceKind::ImportedFile,memory);
        if(secureFile(stateFile))if(const auto state=readJson(stateFile,1024*1024))auth.restore(*state);
        timer.setInterval(250);QObject::connect(&timer,&QTimer::timeout,q,[this]{
            const auto copy=operations;for(const auto& item:copy)if(const auto rejection=item.second->lease.check())finish(item.second,failure(*rejection==Capabilities::Rejection::DeadlineExceeded?"DEADLINE_EXCEEDED":"PERMISSION_DENIED","History invocation expired."));
        });
    }
    ResourceContext context(const QString& client)const{for(const auto& c:access.clients())if(c.id.str()==client)return {client,c.digest};return {};}
    bool valid(const ResourceContext& c)const{return access.enabled()&&!c.principal.isEmpty()&&context(c.principal).credential==c.credential;}
    bool save()const{return writePrivateJson(stateFile,auth.persistentState());}
    void finish(const std::shared_ptr<Operation>& op,QJsonObject payload){
        const auto key=op->peer+'/'+op->id;const auto it=operations.find(key);if(it==operations.end()||it->second!=op)return;
        operations.erase(it);
        const auto authorization=op->lease.checkAuthorization();
        const auto rejection=op->lease.check();const auto finished=op->lease.finish();
        if(!valid(op->context)||authorization)payload=failure("PERMISSION_DENIED","History authorization changed.");
        else if(!finished&&rejection)payload=failure(*rejection==Capabilities::Rejection::DeadlineExceeded?"DEADLINE_EXCEEDED":"CANCELLED","History invocation expired.");
        op->cancelled->store(true);auto done=std::move(op->done);if(operations.empty())timer.stop();done(std::move(payload));
    }
    void list(const std::shared_ptr<Operation>& op){
        const auto now=std::chrono::steady_clock::now();for(auto it=listCursors.begin();it!=listCursors.end();)if(now>=it->second.expires)it=listCursors.erase(it);else ++it;
        QString after;const auto version=access.version(op->context.principal);
        if(!op->request.cursor.isEmpty()) {const auto it=listCursors.find(op->request.cursor);
            if(it==listCursors.end()){finish(op,failure("HISTORY_CURSOR_EXPIRED","Capture cursor expired."));return;}
            if(it->second.context.principal!=op->context.principal||it->second.context.credential!=op->context.credential||it->second.version!=version){finish(op,failure("PERMISSION_DENIED","Capture authorization changed."));return;}
            after=it->second.after;
        }
        auto captures=reads.list(op->context);std::sort(captures.begin(),captures.end(),[](const auto& a,const auto& b){return a.id.text()<b.id.text();});
        QJsonArray rows;QString last;bool more=false;
        for(const auto& capture:captures){if(capture.id.text()<=after)continue;if(rows.size()>=op->request.limit){more=true;break;}
            const auto grant=auth.authorize(op->context,capture.id);
            rows.append(QJsonObject{{"captureId",capture.id.text()},{"createdMs",QString::number(capture.createdMs)},
                {"firstRecordId",QString::number(grant->first)},{"highWaterRecordId",QString::number(capture.highWaterRecordId)},
                {"sourceCompleteness",completeness(capture.integrity)},{"active",capture.active}});last=capture.id.text();}
        QJsonValue cursor=QJsonValue::Null;
        if(more){if(listCursors.size()>=128){finish(op,failure("BUSY","Capture cursor limit reached."));return;}
            const auto token=newId();listCursors.emplace(token,ListCursor{op->context,last,version,now+std::chrono::minutes(1)});cursor=token;}
        if(!op->request.cursor.isEmpty())listCursors.erase(op->request.cursor);
        finish(op,success({{"captures",rows},{"nextCursor",cursor}}));
    }
    void read(const std::shared_ptr<Operation>& op){
        const QPointer<HistoryCapabilityAdapter> guard(owner);
        reads.readAsync({{QUuid(op->request.capture)},op->request.cursor,op->request.maxBytes},op->context,[this,guard,op](HistoryReadPage p){
            if(!guard)return;if(!p.error.isEmpty()){finish(op,failure(p.error,p.error));return;}
            QJsonArray rows;for(const auto& f:p.records){const auto& r=f.record;
                rows.append(QJsonObject{{"recordId",QString::number(r.recordId)},{"lastRecordId",QString::number(r.lastRecordId)},
                    {"byteOffset",int(f.offset)},{"byteCount",int(r.payload.size())},{"bytesBase64",QString::fromLatin1(r.payload.toBase64())},
                    {"kind",r.kind==RecordKind::Payload?"payload":r.kind==RecordKind::Gap?"gap":"boundary"},
                    {"reason",r.reason},{"timestampMs",QString::number(r.wallTimestampMs)}});}
            finish(op,success({{"captureId",op->request.capture},{"records",rows},{"byteCount",int(p.byteCount)},
                {"highWaterRecordId",QString::number(p.highWater)},{"sourceCompleteness",completeness(p.integrity)},
                {"truncated",p.truncated},{"nextCursor",p.cursor.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(p.cursor)}}));
        },op->cancelled);
    }
    void searchHistory(const std::shared_ptr<Operation>& op){
        const QPointer<HistoryCapabilityAdapter> guard(owner);
        search.searchAsync({{QUuid(op->request.capture)},op->request.rawBytes?SearchMode::RawBytes:SearchMode::Text,op->request.query,op->request.cursor,op->request.maxMatches},op->context,
            [this,guard,op](HistorySearchPage p){
                if(!guard)return;if(!p.error.isEmpty()){finish(op,failure(p.error,p.error));return;}
                struct Delivery {HistorySearchPage page;QJsonArray matches;size_t index{0};};
                auto delivery=std::make_shared<Delivery>();delivery->page=std::move(p);addEvidence(op,delivery);
            },op->cancelled);
    }
    template<class Delivery>void addEvidence(const std::shared_ptr<Operation>& op,const std::shared_ptr<Delivery>& delivery){
        if(op->cancelled->load())return;
        if(delivery->index==delivery->page.matches.size()) {
            const auto& p=delivery->page;finish(op,success({{"captureId",op->request.capture},{"matches",delivery->matches},
                {"complete",p.complete},{"highWaterRecordId",QString::number(p.highWater)},{"sourceCompleteness",completeness(p.integrity)},
                {"projectionVersion",p.projectionVersion},{"projectionGap",gapRange(p.projectionGap)},
                {"reason",p.reason.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(p.reason)},
                {"nextCursor",p.cursor.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(p.cursor)}}));return;
        }
        const auto& match=delivery->page.matches[delivery->index];Evidence::Source sourceRef{Evidence::SourceKind::TerminalCapture,op->request.capture,1,{}};
        for(auto span:match.spans)sourceRef.ranges.push_back({span.record,span.offset,span.length});
        const QPointer<HistoryCapabilityAdapter> guard(owner);
        evidence.create(std::move(sourceRef),{op->context.principal,op->context.credential},op->request.rawBytes?0:1,
            [this,guard,op,delivery](Evidence::Result result){
                if(!guard || op->cancelled->load())return;
                if(!result.error.isEmpty()){finish(op,failure(result.error,result.error));return;}
                const auto& ref=*result.reference;
                delivery->matches.append(QJsonObject{{"ranges",ranges(delivery->page.matches[delivery->index].spans)},
                    {"evidenceId",ref.id},{"contentHash",QString::fromLatin1(ref.hash.toHex())},{"persistence","memory"},
                    {"expiresAtMs",QString::number(ref.expiresAtMs)},{"sourceCompleteness",ref.sourceCompleteness},
                    {"projectionVersion",ref.projectionVersion}});++delivery->index;addEvidence(op,delivery);
            });
    }
};
HistoryCapabilityAdapter::HistoryCapabilityAdapter(AccessStore& a,HistoryService* s,Capabilities::CapabilityDispatcher& d,QString f,QObject* p)
    :QObject(p),_impl(std::make_unique<Impl>(this,a,s,d,std::move(f))){}
HistoryCapabilityAdapter::~HistoryCapabilityAdapter(){stop();}
void HistoryCapabilityAdapter::stop(){cancel({});_impl->listCursors.clear();}
void HistoryCapabilityAdapter::cancel(const QString& peer,const QString& id){
    const auto copy=_impl->operations;for(const auto& item:copy){const auto& op=item.second;if(!peer.isEmpty()&&op->peer!=peer)continue;if(!id.isEmpty()&&op->id!=id)continue;
        _impl->operations.erase(item.first);op->cancelled->store(true);op->lease.finish();}
    if(_impl->operations.empty())_impl->timer.stop();
}
std::vector<SharingClient> HistoryCapabilityAdapter::clients()const{std::vector<SharingClient> rows;for(const auto& c:_impl->access.clients())rows.push_back({c.id.str(),c.label});return rows;}
std::optional<ResourceGrant> HistoryCapabilityAdapter::grant(const QString& c,HistoryCaptureId id)const{return _impl->auth.authorize(_impl->context(c),id);}
bool HistoryCapabilityAdapter::share(ResourceGrant g){
    const auto c=_impl->context(g.principal);if(c.principal.isEmpty()||!_impl->reads.descriptor(g.capture))return false;
    g.credential=c.credential;auto old=_impl->auth;if(!_impl->auth.share(g))return false;if(!_impl->save()){_impl->auth=std::move(old);return false;}
    _impl->listCursors.clear();return true;
}
bool HistoryCapabilityAdapter::revoke(const QString& c,HistoryCaptureId id){auto old=_impl->auth;_impl->auth.revoke(c,id);if(!_impl->save()){_impl->auth=std::move(old);return false;}_impl->listCursors.clear();return true;}
std::optional<Capabilities::AuthorizationSnapshot> HistoryCapabilityAdapter::authorization(const QString& client,const Capabilities::HistoryCaptureBinding& binding)const{
    if(binding.generation!=1)return std::nullopt;const auto c=_impl->context(client);const auto g=_impl->reads.authorize(c,{QUuid(binding.id.value())});
    if(!g)return std::nullopt;return Capabilities::AuthorizationSnapshot{binding,g->version};
}
void HistoryCapabilityAdapter::call(QString peer,QString id,HistoryToolRequest request,const Capabilities::McpCapabilityAdapter& entry,QString client,std::function<void(QJsonObject)> done){
    const auto key=peer+'/'+id;if(_impl->operations.count(key)||_impl->operations.size()>=8){done(failure("BUSY","History request limit reached."));return;}
    const auto context=_impl->context(client);if(!_impl->valid(context)){done(failure("PERMISSION_DENIED","Client unavailable."));return;}
    Capabilities::ResourceBinding binding{Capabilities::DirectoryBinding{}};quint64 version=_impl->access.version(client);
    const char* capability="history.capture.list";
    if(request.action!=HistoryToolRequest::Action::List){const auto g=_impl->reads.authorize(context,{QUuid(request.capture)});if(!g){done(failure("PERMISSION_DENIED","Capture is not shared."));return;}
        version=g->version;binding=Capabilities::HistoryCaptureBinding{Capabilities::ResourceId{request.capture},1};capability=request.action==HistoryToolRequest::Action::Read?"history.read":"history.search";}
    auto admission=_impl->dispatcher.admit(Capabilities::CapabilityId{QString::fromLatin1(capability)},entry.context(binding,version,Capabilities::OperationId{newId()},Capabilities::Clock::now()+std::chrono::milliseconds(9500)));
    if(std::holds_alternative<Capabilities::Rejection>(admission)){done(failure("PERMISSION_DENIED","History admission refused."));return;}
    auto op=std::make_shared<Impl::Operation>();op->peer=std::move(peer);op->id=std::move(id);op->request=std::move(request);op->context=context;op->done=std::move(done);
    op->lease=std::move(std::get<Capabilities::InvocationLease>(admission));op->cancelled=std::make_shared<std::atomic<bool>>(false);
    _impl->operations.emplace(key,op);_impl->timer.start();
    if(op->request.action==HistoryToolRequest::Action::List)_impl->list(op);else if(op->request.action==HistoryToolRequest::Action::Read)_impl->read(op);else _impl->searchHistory(op);
}
}
