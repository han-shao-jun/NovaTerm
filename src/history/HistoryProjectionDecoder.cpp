#include "HistoryProjectionDecoder.h"
#include <algorithm>
namespace NovaTerm::Recording {
void appendSpan(std::vector<RawSpan>& spans,RawSpan span) {
    if (!span.length) return;
    if (!spans.empty() && spans.back().record==span.record) {
        auto& last=spans.back();
        if (span.offset>=last.offset && span.offset<=last.offset+last.length) {
            last.length=(std::max)(last.length,span.offset+span.length-last.offset); return;
        }
    }
    spans.push_back(span);
}
HistoryLiteralMatcher::HistoryLiteralMatcher(QByteArray q):_query(std::move(q)),_prefix(size_t(_query.size())) {
    for (int i=1,j=0;i<_query.size();++i) {
        while(j && _query[i]!=_query[j]) j=_prefix[size_t(j-1)];
        if(_query[i]==_query[j]) ++j; _prefix[size_t(i)]=j;
    }
}
std::optional<HistoryMatch> HistoryLiteralMatcher::feed(char c,RawSpan origin) {
    if (_query.isEmpty()) return std::nullopt;
    while (_matched && c!=_query[_matched]) _matched=_prefix[size_t(_matched-1)];
    if(c==_query[_matched]) ++_matched;
    _origins.push_back(origin); while(_origins.size()>size_t(_query.size())) _origins.pop_front();
    if(_matched!=_query.size()) return std::nullopt;
    HistoryMatch result; for(auto s:_origins) appendSpan(result.spans,s);
    _matched=0; _origins.clear(); return result;
}
void HistoryLiteralMatcher::reset() { _matched=0;_origins.clear(); }
void HistoryProjectionDecoder::glyph(uint cp,std::vector<RawSpan> spans) {
    Glyph g{cp,std::move(spans)};
    if (_column<_line.size()) _line[_column]=std::move(g); else _line.push_back(std::move(g));
    ++_column;
}
void HistoryProjectionDecoder::pending(bool final) {
    while(!_pending.isEmpty()) {
        const auto lead=static_cast<unsigned char>(_pending[0]); int count=lead<0x80 ? 1 : lead>=0xc2 && lead<=0xdf ? 2 : lead>=0xe0 && lead<=0xef ? 3 : lead>=0xf0 && lead<=0xf4 ? 4 : 0;
        uint cp=count==1 ? lead : count ? lead&((1u<<(7-count))-1) : 0xfffd;
        bool invalid=!count;
        for(int i=1;i<(std::min)(count,int(_pending.size()));++i) {
            const auto b=static_cast<unsigned char>(_pending[i]); if((b&0xc0)!=0x80) {invalid=true;break;} cp=(cp<<6)|(b&63);
        }
        if(!invalid && _pending.size()<count && !final) return;
        if(!invalid && _pending.size()>=count && ((count==2 && cp<0x80)||(count==3 && cp<0x800)
            ||(count==4 && cp<0x10000)||cp>0x10ffff||(cp>=0xd800 && cp<=0xdfff))) invalid=true;
        if(invalid || _pending.size()<count) {cp=0xfffd;count=1;}
        std::vector<RawSpan> spans; for(int i=0;i<count;++i) appendSpan(spans,_pendingOrigins[size_t(i)]);
        glyph(cp,std::move(spans)); _pending.remove(0,count); _pendingOrigins.erase(_pendingOrigins.begin(),_pendingOrigins.begin()+count);
    }
}
void HistoryProjectionDecoder::line(const std::function<void(Unit)>& output) {
    for(auto& g:_line) { const char32_t cp=g.codepoint; output({QString::fromUcs4(&cp,1).toUtf8(),std::move(g.spans)}); }
    _line.clear(); _column=0; _rawLineBytes=0;
}
bool HistoryProjectionDecoder::feed(char c,RawSpan origin,const std::function<void(Unit)>& output) {
    appendSpan(_limit,origin);
    // The additional column/fragment bounds keep all decoder/matcher/cache state below 1 MiB.
    if(++_rawLineBytes>64*1024 || _line.size()>4096 || _limit.size()>4096) return false;
    const auto b=static_cast<unsigned char>(c);
    if(_escape!=Escape::None) {
        if(++_sequenceBytes>4096) return false;
        switch(_escape) {
        case Escape::Esc: _escape=c=='['?Escape::Csi:c==']'?Escape::Osc:Escape::None; break;
        case Escape::Csi: if(b>=0x40 && b<=0x7e) _escape=Escape::None; break;
        case Escape::Osc: if(b==7) _escape=Escape::None; else if(b==27) _escape=Escape::OscEsc; break;
        case Escape::OscEsc: _escape=c=='\\'?Escape::None:Escape::Osc; break;
        case Escape::None: break;
        }
        return true;
    }
    if(b==27) {pending(true); _escape=Escape::Esc;_sequenceBytes=1;return true;}
    if(c=='\r') {pending(true);_column=0;return true;}
    if(c=='\b') {pending(true);if(_column) --_column;return true;}
    if(c=='\n') {pending(true);line(output);output({"\n",{origin}});_limit.clear();return true;}
    if(b<32 && c!='\t') {pending(true);return true;}
    _pending.append(c);_pendingOrigins.push_back(origin);pending(false);return _line.size()<=4096;
}
void HistoryProjectionDecoder::flush(const std::function<void(Unit)>& output) {pending(true);line(output);}
void HistoryProjectionDecoder::reset() { _line.clear();_pending.clear();_pendingOrigins.clear();_limit.clear();_column=0;_sequenceBytes=_rawLineBytes=0;_escape=Escape::None; }
}
