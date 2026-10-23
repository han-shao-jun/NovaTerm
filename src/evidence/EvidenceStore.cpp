#include "EvidenceStore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QPointer>
#include <QUuid>
namespace NovaTerm::Evidence {
bool EvidenceStore::registerSource(SourceKind kind,std::shared_ptr<EvidenceSourceReader> source){return source && _sources.emplace(kind,std::move(source)).second;}
void EvidenceStore::sweep(){const auto now=std::chrono::steady_clock::now();for(auto it=_references.begin();it!=_references.end();)if(now>=it->second.expires)it=_references.erase(it);else ++it;}
void EvidenceStore::create(Source source,Context context,int version,std::function<void(Result)> done){access(std::move(source),std::move(context),{},version,std::move(done));}
void EvidenceStore::resolve(const QString& id,Context context,std::function<void(Result)> done){
    sweep();const auto it=_references.find(id);if(it==_references.end()){done({{}, {},"EVIDENCE_EXPIRED"});return;}
    access(it->second.ref.source,std::move(context),it->second,0,std::move(done));
}
void EvidenceStore::access(Source source,Context context,std::optional<Entry> expected,int version,std::function<void(Result)> done){
    sweep();const auto found=_sources.find(source.kind);
    if(found==_sources.end()){done({{}, {},"EVIDENCE_EXPIRED"});return;}
    const auto reader=found->second;
    if(expected && (expected->owner.principal!=context.principal || expected->owner.credential!=context.credential)){done({{}, {},"PERMISSION_DENIED"});return;}
    if(!reader->available(source)){done({{}, {},"EVIDENCE_EXPIRED"});return;}
    if(context.principal.isEmpty() || context.credential.isEmpty() || !reader->authorized(source,context)
        || (expected && (expected->owner.principal!=context.principal || expected->owner.credential!=context.credential))){done({{}, {},"PERMISSION_DENIED"});return;}
    quint64 size=0;for(const auto& range:source.ranges)size+=range.length;
    if(source.ranges.empty() || source.ranges.size()>128 || size>16384 || !source.generation){done({{}, {},"RESULT_LIMIT"});return;}
    if(_pending>=8 || (!expected && _references.size()+_pending>=512)){done({{}, {},"BUSY"});return;}
    ++_pending;QPointer<EvidenceStore> guard(this);
    reader->read(source,context,[this,guard,reader,source,context,expected,version,done](SourceResult result){
        if(!guard)return;--_pending;
        if(!reader->available(source)){done({{}, {},"EVIDENCE_EXPIRED"});return;}
        if(!reader->authorized(source,context)){done({{}, {},"PERMISSION_DENIED"});return;}
        quint64 size=0;for(auto r:source.ranges)size+=r.length;
        if(!result.error.isEmpty() || quint64(result.bytes.size())!=size){done({{}, {},result.error.isEmpty()?QString("EVIDENCE_EXPIRED"):result.error});return;}
        const auto hash=QCryptographicHash::hash(result.bytes,QCryptographicHash::Sha256);
        if(expected && (std::chrono::steady_clock::now()>=expected->expires || expected->ref.hash!=hash)){done({{}, {},"EVIDENCE_EXPIRED"});return;}
        Reference ref;
        if(expected)ref=expected->ref;
        else{
            for(const auto& item:_references){const auto& entry=item.second;const auto& prior=entry.ref;
                if(entry.owner.principal!=context.principal || entry.owner.credential!=context.credential || prior.source.kind!=source.kind
                    || prior.source.resource!=source.resource || prior.source.generation!=source.generation || prior.hash!=hash
                    || prior.projectionVersion!=version || prior.source.ranges.size()!=source.ranges.size()
                    || std::chrono::steady_clock::now()>=entry.expires)continue;
                bool same=true;for(size_t i=0;i<source.ranges.size();++i){const auto& a=prior.source.ranges[i];const auto& b=source.ranges[i];same= same && a.record==b.record && a.offset==b.offset && a.length==b.length;}
                if(same){done({prior,std::move(result.bytes),{}});return;}
            }
            ref.id=QUuid::createUuid().toString(QUuid::WithoutBraces);ref.source=source;ref.hash=hash;ref.collectedMs=QDateTime::currentMSecsSinceEpoch();ref.expiresAtMs=ref.collectedMs+300000;
            ref.sourceCompleteness=result.completeness;ref.projectionVersion=version;ref.provenance=result.provenance;
            _references.emplace(ref.id,Entry{ref,context,std::chrono::steady_clock::now()+std::chrono::minutes(5)});}
        done({ref,std::move(result.bytes),{}});
    });
}
}
