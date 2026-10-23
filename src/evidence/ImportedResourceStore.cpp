#include "ImportedResourceStore.h"
#include <QUuid>
#include <QJsonDocument>
namespace NovaTerm::Evidence {
void ImportedResourceStore::sweep(){const auto now=std::chrono::steady_clock::now();for(auto it=_resources.begin();it!=_resources.end();)if(now>=it->second.expires)it=_resources.erase(it);else ++it;}
std::optional<Source> ImportedResourceStore::publish(QByteArray bytes,Context owner,QJsonObject provenance,SourceKind kind){
    sweep();quint64 used=0;for(const auto& r:_resources)used+=quint64(r.second.bytes.size());
    if(kind==SourceKind::TerminalCapture || owner.principal.isEmpty() || owner.credential.isEmpty() || bytes.isEmpty() || bytes.size()>8*1024*1024
        || used+quint64(bytes.size())>32*1024*1024 || _resources.size()>=64 || provenance.size()>32
        || QJsonDocument(provenance).toJson(QJsonDocument::Compact).size()>8192)return std::nullopt;
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);const auto size=quint32(bytes.size());
    _resources.emplace(id,Entry{std::move(bytes),std::move(owner),std::move(provenance),kind,std::chrono::steady_clock::now()+std::chrono::minutes(5)});
    return Source{kind,id,1,{{0,0,size}}};
}
void ImportedResourceStore::remove(const QString& resource){_resources.erase(resource);}
bool ImportedResourceStore::available(const Source& s)const{
    const auto it=_resources.find(s.resource);return it!=_resources.end() && s.generation==1 && it->second.kind==s.kind
        && std::chrono::steady_clock::now()<it->second.expires;
}
bool ImportedResourceStore::authorized(const Source& s,const Context& c)const{
    const auto it=_resources.find(s.resource);if(it==_resources.end() || s.generation!=1 || it->second.kind!=s.kind)return false;
    const auto& r=it->second;
    if(c.principal!=r.owner.principal || c.credential!=r.owner.credential || std::chrono::steady_clock::now()>=r.expires)return false;
    for(auto range:s.ranges)if(range.record || quint64(range.offset)+range.length>quint64(r.bytes.size()))return false;
    return true;
}
void ImportedResourceStore::read(const Source& s,const Context& c,Done done){
    if(!authorized(s,c)){done({{},"PERMISSION_DENIED",{}, {}});return;}
    const auto& r=_resources.at(s.resource);QByteArray bytes;for(auto range:s.ranges){if(bytes.size()+range.length>16384){done({{},"RESULT_LIMIT",{}, {}});return;}bytes+=r.bytes.mid(range.offset,range.length);}
    done({std::move(bytes),{},"complete",r.provenance});
}
}
