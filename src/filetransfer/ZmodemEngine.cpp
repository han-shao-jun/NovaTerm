/** @file ZmodemEngine.cpp @brief 协商、异步文件提交、位置重传和 OO 收尾。 */
#include "ZmodemEngine.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <utility>
namespace NovaTerm::FileTransfer {
namespace {
constexpr std::uint32_t CanFdx=0x01000000U, CanCrc32=0x20000000U;
constexpr std::uint32_t EscapeControls=0x40000000U;
bool parseFile(const Bytes& bytes,FileInfo& file)
{
    const auto zero=std::find(bytes.begin(),bytes.end(),0);
    if(zero==bytes.end()) return false;
    file.name.assign(bytes.begin(),zero);
    if(!validFileNameUtf8(file.name)) return false;
    const auto meta=zero+1;
    const auto end=std::find(meta,bytes.end(),0);
    if(end==bytes.end()) return false;
    for(auto iterator=meta;iterator!=end;++iterator)
        if((*iterator<'0' || *iterator>'9') && *iterator!=' ') return false;
    const auto separator=std::find(meta,end,' ');
    if(meta==separator) { file.size.reset(); return true; }
    const std::string sizeText(meta,separator);
    std::uint64_t value=0;
    const auto parsed=std::from_chars(sizeText.data(),sizeText.data()+sizeText.size(),value);
    if(parsed.ec!=std::errc{} || parsed.ptr!=sizeText.data()+sizeText.size()
       || value>MaxFileSize) return false;
    file.size=value; return true;
}
void append(Bytes& into,Bytes bytes)
{ into.insert(into.end(),bytes.begin(),bytes.end()); }
}
ZHeaderFormat ZmodemEngine::binaryFormat() const
{ return _crc32 ? ZHeaderFormat::Binary32 : ZHeaderFormat::Binary16; }
bool ZmodemEngine::onStart(TimePoint now)
{
    _codec.reset(); _lastPacket.clear(); _file={}; _offset=0; _sentHighWater=0;
    _fileConfirmed=0; _metadataBytes=0; _chunkSize=1024;
    _crc32=_request.config.zmodemCrc32; _receiveActive=false; _dataCrc32=false;
    _awaitDrain=false; _ooCount=0; _closingEnd.reset();
    for(const auto& file:_request.files) {
        if(!validFileNameUtf8(file.name)) { fail(Error::InvalidRequest); return false; }
    }
    if(_request.direction==Direction::Send) {
        _phase=Phase::WaitInit;
        sendPacket(ZmodemCodec::header(ZFrame::RequestInit,0,ZHeaderFormat::Hex),now);
    } else { _phase=Phase::ReceiveHeader; sendInit(now); }
    return !terminal(_progress.state);
}
void ZmodemEngine::sendPacket(Bytes bytes,TimePoint now)
{
    _lastPacket=std::move(bytes);
    clearDeadline();
    if(emitBytes(_lastPacket)) {
        _awaitDrain=true;
        // 空输出不能等待通道的排空回调。
        if(_lastPacket.empty()) onOutputDrained(now);
    }
}
void ZmodemEngine::onOutputDrained(TimePoint now)
{
    if(!_awaitDrain) return;
    _awaitDrain=false;
    const auto interval=_progress.state==State::Negotiating
        ? _request.config.handshakeRetryMs : _request.config.responseTimeoutMs;
    if(_phase==Phase::WaitOO) {
        if(!_closingEnd) {
            const auto duration=_request.config.closingTimeoutMs;
            _closingEnd=duration>std::numeric_limits<TimePoint>::max()-now
                ? std::numeric_limits<TimePoint>::max() : now+duration;
        }
        _deadline=_closingEnd;
    } else setDeadline(now,interval);
}
void ZmodemEngine::sendInit(TimePoint now)
{
    // 宣告 8 KiB 缓冲，但不宣告 CANOVIO；文件操作期间暂停输入消费。
    const auto flags=CanFdx|EscapeControls|(_request.config.zmodemCrc32 ? CanCrc32 : 0U);
    sendPacket(ZmodemCodec::header(ZFrame::ReceiveInit,8192U|flags,ZHeaderFormat::Hex),now);
}
void ZmodemEngine::sendResume(TimePoint now)
{
    _codec.reset(); _phase=Phase::ReceiveHeader;
    sendPacket(ZmodemCodec::header(ZFrame::ResumePosition,static_cast<std::uint32_t>(_offset),
                                ZHeaderFormat::Hex),now);
}
void ZmodemEngine::onByte(std::uint8_t byte,TimePoint now)
{
    if(_phase==Phase::WaitOO) {
        if(byte=='O') ++_ooCount; else _ooCount=0;
        if(_ooCount==2) { clearDeadline(); _progress.state=State::Completed; return; }
        if(auto item=_codec.feed(byte)) {
            if(item->kind==ZEventKind::Cancel) { cancel(now); return; }
            if(item->kind==ZEventKind::Header && item->frame==ZFrame::Finish && retry())
                sendPacket(ZmodemCodec::header(ZFrame::Finish,0,ZHeaderFormat::Hex),now);
        }
        return;
    }
    if(auto item=_codec.feed(byte)) event(std::move(*item),now);
}
void ZmodemEngine::event(ZEvent item,TimePoint now)
{
    if(item.kind==ZEventKind::Cancel) { cancel(now); return; }
    if(item.kind==ZEventKind::Error) {
        if(!retry()) return;
        if(_request.direction==Direction::Receive && _receiveActive) sendResume(now);
        else emitBytes(ZmodemCodec::header(ZFrame::Nak,0,ZHeaderFormat::Hex));
        return;
    }
    if(item.kind==ZEventKind::Header) {
        if(item.frame==ZFrame::Command) { fail(Error::UnsupportedVariant); return; }
        if(item.frame==ZFrame::Abort || item.frame==ZFrame::Cancel
           || item.frame==ZFrame::FileError) { cancel(now); return; }
        if(_request.direction==Direction::Receive) receivedHeader(item,now);
        else sentHeader(item,now);
    } else if(_request.direction==Direction::Receive) receivedData(std::move(item),now);
}
void ZmodemEngine::receivedHeader(const ZEvent& item,TimePoint now)
{
    switch(item.frame) {
    case ZFrame::RequestInit:
        if(_progress.state==State::Negotiating || retry()) sendInit(now);
        break;
    case ZFrame::SendInit:
        _phase=Phase::InitData; _codec.expectData(item.crc32,33); break;
    case ZFrame::File:
        _phase=Phase::FileMetadata; _codec.expectData(item.crc32,4096); break;
    case ZFrame::Data:
        if(!_receiveActive) { if(retry()) sendInit(now); break; }
        if(item.position!=_offset) { if(retry()) sendResume(now); break; }
        _phase=Phase::ReceiveData; _dataCrc32=item.crc32;
        _codec.expectData(item.crc32,8192); break;
    case ZFrame::Eof:
        if(!_receiveActive) { if(retry()) sendInit(now); break; }
        if(item.position!=_offset || (_file.size && *_file.size!=_offset)) {
            if(retry()) sendResume(now);
            break;
        }
        clearDeadline(); _phase=Phase::ReceiveHeader; fileAction(ActionKind::FinishFile,now); break;
    case ZFrame::Finish:
        if(_receiveActive) { if(retry()) sendResume(now); break; }
        _progress.state=State::Closing; _phase=Phase::WaitOO; _ooCount=0;
        sendPacket(ZmodemCodec::header(ZFrame::Finish,0,ZHeaderFormat::Hex),now); break;
    case ZFrame::Nak:
        if(retry()) sendPacket(_lastPacket,now);
        break;
    case ZFrame::Challenge:
        // 挑战响应不替换正在等待确认的文件事务及其重试期限。
        emitBytes(ZmodemCodec::header(ZFrame::Ack,item.position,ZHeaderFormat::Hex)); break;
    default: break;
    }
}
void ZmodemEngine::receivedData(ZEvent item,TimePoint now)
{
    if(_phase==Phase::InitData) {
        if(item.end!=ZEnd::Wait || item.bytes.empty() || item.bytes.back()!=0) {
            fail(Error::Protocol); return;
        }
        _phase=Phase::ReceiveHeader;
        sendPacket(ZmodemCodec::header(ZFrame::Ack,0,ZHeaderFormat::Hex),now); return;
    }
    if(_phase==Phase::FileMetadata) {
        if(item.end!=ZEnd::Wait) { fail(Error::Protocol); return; }
        FileInfo incoming;
        if(!parseFile(item.bytes,incoming)) { fail(Error::Protocol); return; }
        if(_receiveActive) {
            if(incoming.name!=_file.name || incoming.size!=_file.size) { fail(Error::Protocol); return; }
            if(retry()) sendResume(now);
            return;
        }
        if(_progress.fileIndex>=MaxFiles || item.bytes.size()>256*1024-_metadataBytes) {
            fail(Error::ResourceLimit); return;
        }
        _metadataBytes+=item.bytes.size(); _file=std::move(incoming); _offset=0;
        _fileConfirmed=0; _progress.lengthKnown=_file.size.has_value();
        _phase=Phase::ReceiveHeader; clearDeadline(); fileAction(ActionKind::OfferFile,now); return;
    }
    if(_phase!=Phase::ReceiveData || !_receiveActive) { fail(Error::Protocol); return; }
    if(item.bytes.size()>MaxFileSize-_offset
       || (_file.size && item.bytes.size()>*_file.size-_offset)) {
        fail(Error::SizeMismatch); return;
    }
    _receivedEnd=item.end;
    if(item.bytes.empty()) {
        if(!retry()) return;
        if(item.end==ZEnd::Continue || item.end==ZEnd::Ack)
            _codec.expectData(_dataCrc32,8192);
        else _phase=Phase::ReceiveHeader;
        if(item.end==ZEnd::Wait || item.end==ZEnd::Ack)
            sendPacket(ZmodemCodec::header(ZFrame::Ack,static_cast<std::uint32_t>(_offset),ZHeaderFormat::Hex),now);
        return;
    }
    TransferAction action; action.kind=ActionKind::WriteAt; action.fileIndex=_progress.fileIndex;
    action.file=_file; action.offset=_offset; action.count=item.bytes.size();
    action.bytes=std::move(item.bytes); clearDeadline(); requestAction(std::move(action),now);
}
void ZmodemEngine::acknowledgePosition(std::uint64_t position)
{
    if(position>_fileConfirmed) {
        _progress.transferredBytes+=position-_fileConfirmed; _fileConfirmed=position; madeProgress();
    }
}
void ZmodemEngine::sendFile(TimePoint now)
{
    _codec.reset(); _offset=0; _sentHighWater=0; _fileConfirmed=0;
    if(_progress.fileIndex==_request.files.size()) {
        _phase=Phase::WaitFinish; _progress.state=State::Finishing;
        sendPacket(ZmodemCodec::header(ZFrame::Finish,0,ZHeaderFormat::Hex),now); return;
    }
    _file=_request.files[_progress.fileIndex]; _progress.lengthKnown=true;
    Bytes metadata(_file.name.begin(),_file.name.end()); metadata.push_back(0);
    const auto size=std::to_string(*_file.size)+" 0 0 0";
    metadata.insert(metadata.end(),size.begin(),size.end()); metadata.push_back(0);
    auto packet=ZmodemCodec::header(ZFrame::File,0x01000000U,binaryFormat());
    append(packet,ZmodemCodec::data(byteView(metadata),ZEnd::Wait,_crc32));
    _phase=Phase::WaitPosition; _progress.state=State::Transferring;
    sendPacket(std::move(packet),now);
}
void ZmodemEngine::fileAction(ActionKind kind,TimePoint now)
{
    TransferAction action; action.kind=kind; action.fileIndex=_progress.fileIndex;
    action.file=_file; action.offset=_offset; requestAction(std::move(action),now);
}
void ZmodemEngine::readNext(TimePoint now)
{
    clearDeadline();
    if(_offset==*_file.size) { sendEof(now); return; }
    TransferAction action; action.kind=ActionKind::ReadAt; action.fileIndex=_progress.fileIndex;
    action.file=_file; action.offset=_offset;
    action.count=static_cast<std::size_t>(std::min<std::uint64_t>(_chunkSize,*_file.size-_offset));
    _phase=Phase::Reading; requestAction(std::move(action),now);
}
void ZmodemEngine::sendEof(TimePoint now)
{
    _phase=Phase::WaitEofAck;
    sendPacket(ZmodemCodec::header(ZFrame::Eof,static_cast<std::uint32_t>(_offset),binaryFormat()),now);
}
void ZmodemEngine::sentHeader(const ZEvent& item,TimePoint now)
{
    // 文件反馈只能确认通道已接收的完整帧；队列中的旧控制帧不计为进展。
    if(_awaitDrain && _phase!=Phase::WaitInit) return;
    switch(item.frame) {
    case ZFrame::ReceiveInit:
        if(_phase==Phase::WaitInit) {
            _crc32=_request.config.zmodemCrc32 && (item.position&CanCrc32);
            const auto capacity=item.position&0xffffU;
            _chunkSize=capacity ? std::min<std::uint32_t>(1024,capacity) : 1024;
            sendFile(now);
        } else if(_phase==Phase::WaitEofAck) {
            acknowledgePosition(_offset); clearDeadline(); fileAction(ActionKind::FinishFile,now);
        }
        break;
    case ZFrame::ResumePosition:
        if(_phase!=Phase::WaitPosition && _phase!=Phase::WaitAck && _phase!=Phase::WaitEofAck) break;
        if(item.position>_sentHighWater || item.position>*_file.size) { fail(Error::Protocol); break; }
        if(_phase!=Phase::WaitPosition && !retry()) break;
        acknowledgePosition(item.position); _offset=item.position; readNext(now); break;
    case ZFrame::Ack:
        if(_phase==Phase::WaitAck && item.position==_offset) {
            acknowledgePosition(_offset); readNext(now);
        } else if(_phase==Phase::WaitAck && item.position>_offset) fail(Error::Protocol);
        break;
    case ZFrame::Skip:
        if(_phase==Phase::WaitPosition || _phase==Phase::WaitAck || _phase==Phase::WaitEofAck) {
            ++_progress.fileIndex; madeProgress(); sendFile(now);
        }
        break;
    case ZFrame::Finish:
        if(_phase==Phase::WaitFinish) {
            clearDeadline(); emitBytes(Bytes{'O','O'}); _progress.state=State::Completed;
        }
        break;
    case ZFrame::Nak:
        if(retry()) sendPacket(_lastPacket,now);
        break;
    case ZFrame::Challenge:
        // 挑战响应不替换正在等待确认的文件事务及其重试期限。
        emitBytes(ZmodemCodec::header(ZFrame::Ack,item.position,ZHeaderFormat::Hex)); break;
    default: break;
    }
}
void ZmodemEngine::onOperation(const TransferAction& action,OperationResult result,TimePoint now)
{
    switch(action.kind) {
    case ActionKind::OfferFile:
        if(!result.accepted) {
            ++_progress.fileIndex; _phase=Phase::ReceiveHeader;
            sendPacket(ZmodemCodec::header(ZFrame::Skip,0,ZHeaderFormat::Hex),now); return;
        }
        _receiveActive=true; _progress.state=State::Transferring; sendResume(now); break;
    case ActionKind::ReadAt: {
        if(result.bytes.size()!=action.count) { fail(Error::FileIo); return; }
        auto packet=ZmodemCodec::header(ZFrame::Data,static_cast<std::uint32_t>(_offset),binaryFormat());
        append(packet,ZmodemCodec::data(byteView(result.bytes),ZEnd::Wait,_crc32));
        _offset+=result.bytes.size(); _sentHighWater=std::max(_sentHighWater,_offset);
        _phase=Phase::WaitAck; sendPacket(std::move(packet),now); break;
    }
    case ActionKind::WriteAt:
        _offset+=action.bytes.size(); acknowledgePosition(_offset);
        if(_receivedEnd==ZEnd::Continue || _receivedEnd==ZEnd::Ack) {
            _phase=Phase::ReceiveData; _codec.expectData(_dataCrc32,8192);
        } else _phase=Phase::ReceiveHeader;
        if(_receivedEnd==ZEnd::Wait || _receivedEnd==ZEnd::Ack)
            sendPacket(ZmodemCodec::header(ZFrame::Ack,static_cast<std::uint32_t>(_offset),ZHeaderFormat::Hex),now);
        else setDeadline(now,_request.config.responseTimeoutMs);
        break;
    case ActionKind::FinishFile:
        ++_progress.completedFiles; ++_progress.fileIndex; madeProgress();
        if(_request.direction==Direction::Send) sendFile(now);
        else { _receiveActive=false; _phase=Phase::ReceiveHeader; sendInit(now); }
        break;
    }
}
void ZmodemEngine::onTimeout(TimePoint now)
{
    if(_phase==Phase::WaitOO) { _progress.state=State::Completed; return; }
    if(!retry()) return;
    if(_request.direction==Direction::Receive) {
        _codec.reset();
        if(_receiveActive) sendResume(now);
        else { _phase=Phase::ReceiveHeader; sendInit(now); }
    } else sendPacket(_lastPacket,now);
}
} // namespace NovaTerm::FileTransfer
