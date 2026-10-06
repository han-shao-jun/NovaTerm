/** @file ZmodemCodec.cpp @brief CRC16/32、ZDLE 和三种固定头格式。 */
#include "ZmodemCodec.h"
#include "Checksum.h"
#include <algorithm>
#include <utility>
namespace NovaTerm::FileTransfer {
namespace {
constexpr std::uint8_t Pad='*', Dle=0x18;
void escaped(Bytes& out,std::uint8_t byte)
{
    // 主动转义全部低位控制字符，也避免 CR/@ 的网络终端特殊序列。
    if ((byte & 0x60U)==0 || byte==0x7f || byte==0xff) {
        out.push_back(Dle);
        out.push_back(byte==0x7f ? 'l' : byte==0xff ? 'm' : byte^0x40U);
    } else out.push_back(byte);
}
void appendCrc(Bytes& bytes,bool use32)
{
    if(use32) {
        const auto crc=crc32(byteView(bytes));
        for(unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(crc>>(8U*i)));
    } else {
        // 本库更新函数直接按高字节异或；等价于历史 updcrc 再补两个零。
        const auto crc=crc16(byteView(bytes));
        bytes.push_back(static_cast<std::uint8_t>(crc>>8U));
        bytes.push_back(static_cast<std::uint8_t>(crc));
    }
}
bool crcValid(const Bytes& bytes,bool use32)
{
    return use32 ? crc32(byteView(bytes))==0x2144df1cU : crc16(byteView(bytes))==0;
}
int nibble(std::uint8_t byte)
{
    if(byte>='0' && byte<='9') return byte-'0';
    if(byte>='a' && byte<='f') return byte-'a'+10;
    if(byte>='A' && byte<='F') return byte-'A'+10;
    return -1;
}
bool flowControl(std::uint8_t byte)
{ return byte==0x11 || byte==0x13 || byte==0x91 || byte==0x93; }
}
Bytes ZmodemCodec::header(ZFrame frame,std::uint32_t position,ZHeaderFormat format)
{
    Bytes raw{static_cast<std::uint8_t>(frame)};
    for(unsigned i=0;i<4;++i) raw.push_back(static_cast<std::uint8_t>(position>>(8U*i)));
    appendCrc(raw,format==ZHeaderFormat::Binary32);
    Bytes out{Pad};
    if(format==ZHeaderFormat::Hex) out.push_back(Pad);
    out.push_back(Dle);
    out.push_back(format==ZHeaderFormat::Hex ? 'B' : format==ZHeaderFormat::Binary16 ? 'A' : 'C');
    for(auto byte:raw) {
        if(format==ZHeaderFormat::Hex) {
            constexpr char digits[]="0123456789abcdef";
            out.push_back(digits[byte>>4U]); out.push_back(digits[byte&15U]);
        } else escaped(out,byte);
    }
    if(format==ZHeaderFormat::Hex) {
        out.push_back('\r'); out.push_back('\n');
        if(frame!=ZFrame::Finish && frame!=ZFrame::Ack) out.push_back(0x11);
    }
    return out;
}
Bytes ZmodemCodec::data(ByteView payload,ZEnd end,bool use32)
{
    if(payload.size<0 || payload.size>8192 || (!payload.data && payload.size)) return {};
    Bytes raw;
    if(payload.size) raw.assign(payload.data,payload.data+payload.size);
    raw.push_back(static_cast<std::uint8_t>(end));
    appendCrc(raw,use32);
    Bytes out;
    for(isize i=0;i<payload.size;++i) escaped(out,static_cast<std::uint8_t>(payload.data[i]));
    out.push_back(Dle); out.push_back(static_cast<std::uint8_t>(end));
    for(std::size_t i=static_cast<std::size_t>(payload.size)+1;i<raw.size();++i) escaped(out,raw[i]);
    if(end==ZEnd::Wait) out.push_back(0x11);
    return out;
}
void ZmodemCodec::reset()
{
    _mode=Mode::Search; _bytes.clear(); _escape=false; _hexNibble=-1; _cancelCount=0;
    _crcBytes=0;
}
void ZmodemCodec::expectData(bool use32,std::size_t maxBytes)
{
    _mode=Mode::Data; _bytes.clear(); _escape=false; _hexNibble=-1;
    _format=use32 ? ZHeaderFormat::Binary32 : ZHeaderFormat::Binary16;
    _maxData=std::min(maxBytes,std::size_t{8192}); _crcBytes=0;
}
std::optional<ZEvent> ZmodemCodec::error()
{ reset(); return ZEvent{}; }
std::optional<ZEvent> ZmodemCodec::finishHeader()
{
    const bool use32=_format==ZHeaderFormat::Binary32;
    if(!crcValid(_bytes,use32)) return error();
    ZEvent event; event.kind=ZEventKind::Header; event.frame=static_cast<ZFrame>(_bytes[0]);
    event.crc32=use32;
    for(unsigned i=0;i<4;++i) event.position|=static_cast<std::uint32_t>(_bytes[1+i])<<(i*8U);
    reset(); return event;
}
std::optional<ZEvent> ZmodemCodec::finishData()
{
    const bool use32=_format==ZHeaderFormat::Binary32;
    if(!crcValid(_bytes,use32)) return error();
    _bytes.resize(_bytes.size()-(use32 ? 5 : 3));
    ZEvent event; event.kind=ZEventKind::Data; event.bytes=std::move(_bytes);
    event.end=_end; event.crc32=use32;
    reset(); return event;
}
std::optional<ZEvent> ZmodemCodec::decoded(std::uint8_t byte)
{
    _bytes.push_back(byte);
    if(_mode==Mode::Header && _bytes.size()==(_format==ZHeaderFormat::Binary32 ? 9U : 7U)) {
        if(_format==ZHeaderFormat::Hex) { _mode=Mode::HexCr; return {}; }
        return finishHeader();
    }
    if(_mode==Mode::Data && _bytes.size()>_maxData) return error();
    if(_mode==Mode::DataCrc && ++_crcBytes==(_format==ZHeaderFormat::Binary32 ? 4U : 2U))
        return finishData();
    return {};
}
std::optional<ZEvent> ZmodemCodec::feed(std::uint8_t byte)
{
    if(byte==Dle) ++_cancelCount; else _cancelCount=0;
    if(_cancelCount==5) { reset(); ZEvent event; event.kind=ZEventKind::Cancel; return event; }
    if(flowControl(byte)) return {};
    switch(_mode) {
    case Mode::Search: if((byte&0x7fU)==Pad) _mode=Mode::Pad; return {};
    case Mode::Pad:
        if(byte==Dle) _mode=Mode::Indicator;
        else if((byte&0x7fU)!=Pad) _mode=Mode::Search;
        return {};
    case Mode::Indicator:
        if(byte!='A' && byte!='B' && byte!='C') return error();
        _format=byte=='A' ? ZHeaderFormat::Binary16 : byte=='C' ? ZHeaderFormat::Binary32 : ZHeaderFormat::Hex;
        _mode=Mode::Header; _bytes.clear(); _hexNibble=-1; _escape=false; return {};
    case Mode::HexCr:
        if((byte&0x7fU)=='\r') { _mode=Mode::HexLf; return {}; }
        if((byte&0x7fU)=='\n') return finishHeader();
        return error();
    case Mode::HexLf: return (byte&0x7fU)=='\n' ? finishHeader() : error();
    case Mode::Header:
        if(_format==ZHeaderFormat::Hex) {
            const int value=nibble(byte&0x7fU);
            if(value<0) return error();
            if(_hexNibble<0) { _hexNibble=value; return {}; }
            const auto assembled=static_cast<std::uint8_t>((_hexNibble<<4)|value);
            _hexNibble=-1; return decoded(assembled);
        }
        break;
    case Mode::Data: case Mode::DataCrc: break;
    }
    if(!_escape) {
        if(byte==Dle) { _escape=true; return {}; }
        return decoded(byte);
    }
    if(byte==Dle) return {}; // 连续 CAN 留给统一的五字节取消判定。
    _escape=false;
    if(_mode==Mode::Data && byte>='h' && byte<='k') {
        _end=static_cast<ZEnd>(byte); _bytes.push_back(byte); _mode=Mode::DataCrc; return {};
    }
    if(byte=='l') return decoded(0x7f);
    if(byte=='m') return decoded(0xff);
    if((byte&0x60U)==0x40U) return decoded(byte^0x40U);
    return error();
}
} // namespace NovaTerm::FileTransfer
