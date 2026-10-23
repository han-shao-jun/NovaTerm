#include "HistoryToolCodec.h"
#include "ToolCodec.h"
#include <QUuid>
namespace NovaTerm::Mcp {
const QJsonArray& HistoryToolCodec::catalog(){
    static const QJsonArray result=[] {
        const auto string=[](int n){return QJsonObject{{"type","string"},{"minLength",1},{"maxLength",n}};};
        const auto number=[](int low,int high,int fallback){return QJsonObject{{"type","integer"},{"minimum",low},{"maximum",high},{"default",fallback}};};
        const auto tool=[](const char* name,const char* description,QJsonObject properties,QJsonArray required,QJsonObject data){
            const QJsonObject outputProperties{{"schemaVersion",QJsonObject{{"const",1}}},{"ok",QJsonObject{{"type","boolean"}}},
                {"data",data},{"error",QJsonObject{{"type","object"}}}};
            const QJsonObject output{{"type","object"},{"required",QJsonArray{"schemaVersion","ok"}},{"properties",outputProperties}};
            const QJsonObject input{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
            const QJsonObject annotations{{"readOnlyHint",true},{"destructiveHint",false},{"idempotentHint",true},{"openWorldHint",false}};
            return QJsonObject{{"name",name},{"description",description},{"inputSchema",input},{"outputSchema",output},{"annotations",annotations}};
        };
        const QJsonObject text{{"type","string"}},boolean{{"type","boolean"}},array{{"type","array"}};
        const QJsonObject nullableText{{"type",QJsonArray{"string","null"}}};
        const auto data=[](QJsonObject p){return QJsonObject{{"type","object"},{"properties",p}};};
        QJsonArray tools;
        tools.append(tool("novaterm_list_captures","List only explicitly shared persistent receive captures. Live Session grants do not authorize history.",
            {{"limit",number(1,64,32)},{"cursor",string(64)}},{},data({{"captures",array},{"nextCursor",nullableText}})));
        const QJsonObject identity{{"captureId",string(36)},{"cursor",string(64)}};
        auto read=identity;read.insert("maxBytes",number(1,16384,16384));
        tools.append(tool("novaterm_read_history","Read authorized raw receive bytes as base64 at a fixed committed high-water. Gaps and unknown tails remain explicit.",read,{"captureId"},
            data({{"captureId",text},{"records",array},{"byteCount",QJsonObject{{"type","integer"}}},{"highWaterRecordId",text},{"sourceCompleteness",text},{"truncated",boolean},{"nextCursor",nullableText}})));
        auto search=identity;search.insert("mode",QJsonObject{{"type","string"},{"enum",QJsonArray{"text","raw_bytes"}},{"default","text"}});
        search.insert("query",string(5464));search.insert("maxMatches",number(1,8,8));
        tools.append(tool("novaterm_search_history","Search a fixed authorized committed prefix. text is UTF-8 with ANSI stripping and CR overwrite; raw_bytes query is canonical base64. Literal matching only; incomplete pages require continuation. Evidence expires after five minutes.",
            search,{"captureId","query"},data({{"captureId",text},{"matches",array},{"complete",boolean},{"sourceCompleteness",text},{"projectionVersion",QJsonObject{{"type","integer"}}},{"highWaterRecordId",text},{"projectionGap",QJsonObject{{"type",QJsonArray{"object","null"}}}},{"reason",nullableText},{"nextCursor",nullableText}})));
        return tools;
    }();return result;
}
bool HistoryToolCodec::known(const QString& name){for(const auto& t:catalog())if(t.toObject().value("name")==name)return true;return false;}
Result<HistoryToolRequest> HistoryToolCodec::decode(const QString& name,const QJsonObject& args){
    const auto invalid=ToolCodec::validate(name,args);if(!invalid.isEmpty())return Error{ErrorCode::InvalidArgument,invalid};
    HistoryToolRequest r;r.cursor=args.value("cursor").toString();
    if(name=="novaterm_list_captures"){r.limit=args.value("limit").toInt(32);return r;}
    r.capture=args.value("captureId").toString();const auto uuid=QUuid(r.capture);
    if(uuid.isNull() || uuid.toString(QUuid::WithoutBraces)!=r.capture)return Error{ErrorCode::InvalidArgument,"Invalid captureId."};
    if(name=="novaterm_read_history"){r.action=HistoryToolRequest::Action::Read;r.maxBytes=args.value("maxBytes").toInt(16384);return r;}
    if(name!="novaterm_search_history")return Error{ErrorCode::InvalidArgument,"Unknown history tool."};
    r.action=HistoryToolRequest::Action::Search;r.maxMatches=args.value("maxMatches").toInt(8);r.rawBytes=args.value("mode")=="raw_bytes";
    const auto query=args.value("query").toString().toUtf8();
    if(r.rawBytes){const auto decoded=QByteArray::fromBase64Encoding(query,QByteArray::AbortOnBase64DecodingErrors);
        if(!decoded || decoded.decoded.toBase64()!=query)return Error{ErrorCode::InvalidArgument,"raw_bytes query must be canonical base64."};r.query=decoded.decoded;
    }else r.query=query;
    if(r.query.isEmpty() || r.query.size()>4096)return Error{ErrorCode::InvalidArgument,"History query exceeds byte budget."};
    return r;
}
}
