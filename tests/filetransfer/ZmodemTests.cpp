/** @file ZmodemTests.cpp @brief ZMODEM 校验、异步提交与状态机回归。 */
#include "filetransfer/ZmodemCodec.h"
#include "filetransfer/ZmodemEngine.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
using namespace NovaTerm::FileTransfer;
namespace {
void check(bool ok, const char* message)
{ if (!ok) { std::cerr << message << '\n'; std::exit(1); } }
Bytes drain(ITransferEngine& engine)
{
    const auto view = engine.pendingOutput();
    Bytes result;
    if (view.size) result.assign(view.data, view.data + view.size);
    check(engine.acknowledgeOutput(result.size(), 0), "output acknowledgement");
    return result;
}
void feed(ITransferEngine& engine, const Bytes& bytes)
{
    auto frame=bytes;
    if(!frame.empty() && frame.back()==0x11) frame.pop_back();
    check(engine.consume(byteView(frame), 0).consumed == frame.size(), "full consume");
}
void codecTests()
{
    Bytes payload;
    for (unsigned i=0; i<256; ++i) payload.push_back(static_cast<std::uint8_t>(i));
    for (const auto format : {ZHeaderFormat::Hex, ZHeaderFormat::Binary16,
                              ZHeaderFormat::Binary32}) {
        ZmodemCodec codec;
        const auto encoded = ZmodemCodec::header(ZFrame::Data, 0x12345678U, format);
        std::optional<ZEvent> event;
        for (auto byte : encoded) if (auto item = codec.feed(byte)) event = std::move(item);
        check(event && event->kind == ZEventKind::Header, "header fragmented decode");
        check(event->frame == ZFrame::Data && event->position == 0x12345678U,
              "header position little endian");
        codec.expectData(format == ZHeaderFormat::Binary32, 8192);
        auto data = ZmodemCodec::data(byteView(payload), ZEnd::Wait,
                                    format == ZHeaderFormat::Binary32);
        event.reset();
        for (auto byte : data) if (auto item = codec.feed(byte)) event = std::move(item);
        check(event && event->kind == ZEventKind::Data && event->bytes == payload,
              "all bytes escaped roundtrip");
        check(event->end == ZEnd::Wait, "end marker decoded");
        codec.expectData(format == ZHeaderFormat::Binary32, 8192);
        data[1] ^= 1;
        event.reset();
        for (auto byte : data) if (auto item=codec.feed(byte)) event=std::move(item);
        check(event && event->kind == ZEventKind::Error, "payload CRC rejection");
    }
    const Bytes expected{'*','*',0x18,'B','0','0','0','0','0','0','0','0','0','0',
                         '0','0','0','0','\r','\n',0x11};
    check(ZmodemCodec::header(ZFrame::RequestInit,0,ZHeaderFormat::Hex)==expected,
          "known ZRQINIT wire vector");
}
void committedOffsetTests()
{
    ZmodemEngine receiver;
    TransferRequest request;
    check(receiver.start(request,0), "receive start"); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::File,0,ZHeaderFormat::Binary32));
    const Bytes info{'a',0,'3',' ','0',' ','0',0};
    feed(receiver,ZmodemCodec::data(byteView(info),ZEnd::Wait,true));
    auto offer=receiver.takeAction();
    check(offer && offer->kind==ActionKind::OfferFile && offer->file.size==3,
          "explicit offer");
    check(receiver.completeOperation(offer->id,{},0), "offer acceptance"); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Data,0,ZHeaderFormat::Binary32));
    const Bytes payload{0,0x18,0x1a};
    feed(receiver,ZmodemCodec::data(byteView(payload),ZEnd::Wait,true));
    check(receiver.progress().transferredBytes==0 && receiver.pendingOutput().empty(),
          "write not yet confirmed");
    auto write=receiver.takeAction();
    check(write && write->kind==ActionKind::WriteAt && write->offset==0
          && write->bytes==payload,"write exact binary payload");
    check(receiver.consume(byteView(Bytes{1,2}),0).consumed==0,"write pauses consume");
    check(receiver.completeOperation(write->id,{},0),"write success");
    check(receiver.progress().transferredBytes==3,"commit advances offset"); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Data,0,ZHeaderFormat::Hex));
    auto reply=drain(receiver);
    ZmodemCodec decoder; std::optional<ZEvent> event;
    for(auto byte:reply) if(auto item=decoder.feed(byte)) event=std::move(item);
    check(event && event->frame==ZFrame::ResumePosition && event->position==3,
          "old data position corrected");
    feed(receiver,ZmodemCodec::header(ZFrame::Eof,3,ZHeaderFormat::Hex));
    auto finish=receiver.takeAction();
    check(finish && finish->kind==ActionKind::FinishFile,"finish requested");
    check(receiver.progress().completedFiles==0,"finish host confirmation required");
    receiver.completeOperation(finish->id,{},0); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Finish,0,ZHeaderFormat::Hex)); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Finish,0,ZHeaderFormat::Hex));
    check(!receiver.pendingOutput().empty(),"duplicate ZFIN gets bounded reply"); drain(receiver);
    const Bytes tail{'O','O','t','e','x','t'};
    check(receiver.consume(byteView(tail),0).consumed==2,"OO consumes exact prefix");
    check(receiver.progress().state==State::Completed,"receive finished");
}
ZEvent response(ITransferEngine& engine)
{
    ZmodemCodec codec; std::optional<ZEvent> result;
    for(auto byte:drain(engine)) if(auto item=codec.feed(byte)) result=std::move(item);
    check(result && result->kind==ZEventKind::Header,"response header expected");
    return std::move(*result);
}
void offer(ZmodemEngine& receiver, const Bytes& info, bool accept=true)
{
    feed(receiver,ZmodemCodec::header(ZFrame::File,0,ZHeaderFormat::Binary16));
    feed(receiver,ZmodemCodec::data(byteView(info),ZEnd::Wait,false));
    auto action=receiver.takeAction();
    check(action && action->kind==ActionKind::OfferFile,"file offered");
    OperationResult result; result.accepted=accept;
    receiver.completeOperation(action->id,std::move(result),0);
}
void errorTests()
{
    ZmodemEngine receiver;
    TransferRequest receive;
    check(receiver.start(receive,0),"error receive start"); drain(receiver);
    offer(receiver,Bytes{'f',0,'3',0}); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Data,0,ZHeaderFormat::Binary16));
    const Bytes payload{'A','O','O'};
    auto damaged=ZmodemCodec::data(byteView(payload),ZEnd::Wait,false);
    damaged[0]^=1; feed(receiver,damaged);
    check(!receiver.takeAction() && receiver.progress().transferredBytes==0,"CRC error no write");
    auto reply=response(receiver);
    check(reply.frame==ZFrame::ResumePosition && reply.position==0,"CRC error ZRPOS committed offset");
    feed(receiver,ZmodemCodec::header(ZFrame::Data,0,ZHeaderFormat::Binary16));
    feed(receiver,ZmodemCodec::data(byteView(payload),ZEnd::Wait,false));
    auto action=receiver.takeAction(); check(action.has_value(),"corrected packet writes");
    receiver.completeOperation(action->id,{},0); drain(receiver);
    check(receiver.progress().transferredBytes==3,"corrected packet one count");
    feed(receiver,ZmodemCodec::header(ZFrame::File,0,ZHeaderFormat::Binary16));
    feed(receiver,ZmodemCodec::data(byteView(Bytes{'f',0,'3',0}),ZEnd::Wait,false));
    check(!receiver.takeAction(),"duplicate file not offered twice");
    reply=response(receiver);
    check(reply.frame==ZFrame::ResumePosition && reply.position==3,"duplicate metadata position");
    receiver.cancel(0);
    check(receiver.progress().state==State::Cancelled,"cancel state");
    check(!receiver.completeOperation(action->id,{},0),"stale result rejected");
    check(receiver.start(receive,0),"new task starts"); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Command,0,ZHeaderFormat::Hex));
    check(receiver.progress().error==Error::UnsupportedVariant,"ZCOMMAND explicitly rejected");
    check(receiver.start(receive,0),"skip receive starts"); drain(receiver);
    offer(receiver,Bytes{'s',0,'0',0},false);
    check(response(receiver).frame==ZFrame::Skip && receiver.progress().completedFiles==0,
          "explicit decline sends ZSKIP");
    check(receiver.start(receive,0)==false,"active restart rejected"); receiver.cancel(0);
    check(receiver.start(receive,0),"remote cancel start"); drain(receiver);
    const Bytes cancel{0x18,0x18,0x18,0x18,0x18,'x'};
    check(receiver.consume(byteView(cancel),0).consumed==5,"cancel exact prefix");
    check(receiver.progress().state==State::Cancelled,"remote CAN cancels");
}
void duplicateMetadataBudgetTests()
{
    ZmodemEngine receiver; TransferRequest receive;
    receive.config.maxRetries=2;
    receiver.start(receive,0); drain(receiver);
    const Bytes metadata{'r',0,'1',0};
    offer(receiver,metadata); drain(receiver);
    for(unsigned repeat=0;repeat<3;++repeat) {
        feed(receiver,ZmodemCodec::header(ZFrame::File,0,ZHeaderFormat::Binary16));
        feed(receiver,ZmodemCodec::data(byteView(metadata),ZEnd::Wait,false));
        drain(receiver);
    }
    check(receiver.progress().error==Error::RetryLimit,"duplicate metadata has finite budget");
}
void metadataTests()
{
    const std::vector<Bytes> malformed={
        {'x','y'}, {'x',0,'4','2'}, {'x',0,'-','1',0},
        {'x',0,'4','2','9','4','9','6','7','2','9','6',0},
        {0xc0,0x80,0,'0',0}, {0xed,0xa0,0x80,0,'0',0},
        {'x',0,'1',' ','x',0}
    };
    for(const auto& info:malformed) {
        ZmodemEngine receiver; TransferRequest receive;
        receiver.start(receive,0); drain(receiver);
        feed(receiver,ZmodemCodec::header(ZFrame::File,0,ZHeaderFormat::Binary32));
        feed(receiver,ZmodemCodec::data(byteView(info),ZEnd::Wait,true));
        check(receiver.progress().error==Error::Protocol && !receiver.takeAction(),"malformed metadata rejected");
    }
    ZmodemEngine receiver; TransferRequest receive;
    receiver.start(receive,0); drain(receiver);
    offer(receiver,Bytes{'u',0,0}); drain(receiver);
    check(!receiver.progress().lengthKnown,"missing size unknown");
    feed(receiver,ZmodemCodec::header(ZFrame::Data,0,ZHeaderFormat::Hex));
    const Bytes payload{0x1a,0};
    feed(receiver,ZmodemCodec::data(byteView(payload),ZEnd::Wait,false));
    auto action=receiver.takeAction();
    check(action && action->bytes==payload,"unknown length preserves tails");
    receiver.completeOperation(action->id,{},0); drain(receiver);
    feed(receiver,ZmodemCodec::header(ZFrame::Eof,2,ZHeaderFormat::Hex));
    action=receiver.takeAction(); check(action && action->kind==ActionKind::FinishFile,"unknown size finish");
    receiver.completeOperation(action->id,{},0);
}
void senderFeedbackTests()
{
    ZmodemEngine sender; TransferRequest send;
    send.direction=Direction::Send; send.files={{"s",2048}};
    sender.start(send,0); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::ReceiveInit,0x21002000U,ZHeaderFormat::Hex)); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::ResumePosition,0,ZHeaderFormat::Hex));
    auto read=sender.takeAction(); check(read && read->count==1024,"first read bounded");
    OperationResult result; result.bytes.assign(read->count,0x1a);
    sender.completeOperation(read->id,std::move(result),0);
    feed(sender,ZmodemCodec::header(ZFrame::Ack,1024,ZHeaderFormat::Hex));
    check(!sender.takeAction() && sender.progress().transferredBytes==0,
          "undrained ACK cannot confirm unsent bytes");
    feed(sender,ZmodemCodec::header(ZFrame::ResumePosition,512,ZHeaderFormat::Hex));
    check(!sender.takeAction() && sender.progress().transferredBytes==0,
          "undrained ZRPOS cannot advance position");
    const auto packet=drain(sender);
    check(sender.progress().transferredBytes==0,"output not acknowledgement");
    feed(sender,ZmodemCodec::header(ZFrame::Ack,0,ZHeaderFormat::Hex));
    check(!sender.takeAction() && sender.progress().transferredBytes==0,"old ACK no progress");
    feed(sender,ZmodemCodec::header(ZFrame::ResumePosition,512,ZHeaderFormat::Hex));
    read=sender.takeAction(); check(read && read->offset==512 && read->count==1024,"ZRPOS rereads exact offset");
    check(sender.progress().transferredBytes==512,"ZRPOS confirms only prefix");
    result={}; result.bytes.assign(read->count,0x18);
    sender.completeOperation(read->id,std::move(result),0); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::Ack,1536,ZHeaderFormat::Hex));
    read=sender.takeAction(); check(read && read->offset==1536 && read->count==512,"after correction next offset");
    check(sender.progress().transferredBytes==1536,"retransmit not double counted");
    result={}; result.bytes.assign(511,0);
    sender.completeOperation(read->id,std::move(result),0);
    check(sender.progress().error==Error::FileIo,"short read rejected");
    check(sender.start(send,0),"fresh sender restart"); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::ReceiveInit,0x21002000U,ZHeaderFormat::Hex)); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::ResumePosition,1,ZHeaderFormat::Hex));
    check(sender.progress().error==Error::Protocol,"cross task resume refused");
    check(!packet.empty(),"sender packet captured");
}
void closingTests()
{
    ZmodemEngine receiver; TransferRequest receive;
    receiver.start(receive,0); drain(receiver);
    const auto finish=ZmodemCodec::header(ZFrame::Finish,0,ZHeaderFormat::Hex);
    feed(receiver,finish); drain(receiver);
    check(receiver.nextDeadline()==3000,"closing starts after output drain");
    check(receiver.consume(byteView(finish),2000).consumed==finish.size(),"repeat close consumed");
    const auto pending=receiver.pendingOutput();
    receiver.acknowledgeOutput(static_cast<std::size_t>(pending.size),2000);
    check(receiver.nextDeadline()==3000,"repeat FIN does not extend close deadline");
    receiver.advance(3000);
    check(receiver.progress().state==State::Completed,"bounded closing completes without OO");
    check(receiver.consume(byteView(Bytes{'t'}),3000).consumed==0,"completed tail handed back");
    ZmodemEngine sender; TransferRequest send; send.direction=Direction::Send;
    sender.start(send,0); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::ReceiveInit,0x21002000U,ZHeaderFormat::Hex)); drain(sender);
    auto tail=finish; tail.insert(tail.end(),{'t','a','i','l'});
    check(sender.consume(byteView(tail),0).consumed==finish.size(),"sender FIN consumes exact prefix");
    check(sender.progress().state==State::Completed && drain(sender)==Bytes({'O','O'}),"sender sends OO");
}
void metadataBudgetTests()
{
    ZmodemEngine receiver; TransferRequest receive;
    receiver.start(receive,0); drain(receiver);
    Bytes info{'b',0,'0',' '}; info.resize(4095,' '); info.push_back(0);
    for(unsigned index=0;index<64;++index) {
        offer(receiver,info,false); drain(receiver);
    }
    feed(receiver,ZmodemCodec::header(ZFrame::File,0,ZHeaderFormat::Binary16));
    feed(receiver,ZmodemCodec::data(byteView(info),ZEnd::Wait,false));
    check(receiver.progress().error==Error::ResourceLimit,"metadata budget finite");
}
void challengeRetryTests()
{
    ZmodemEngine sender; TransferRequest send;
    send.direction=Direction::Send; send.files={{"challenge",1}};
    sender.start(send,0); drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::ReceiveInit,0x21002000U,ZHeaderFormat::Hex));
    const auto transaction=drain(sender);
    feed(sender,ZmodemCodec::header(ZFrame::Challenge,42,ZHeaderFormat::Hex));
    const auto answer=response(sender);
    check(answer.frame==ZFrame::Ack && answer.position==42,"challenge response echoed");
    sender.advance(10000);
    check(drain(sender)==transaction,"challenge preserves retransmission transaction");
}
void timingAndBudgetTests()
{
    ZmodemEngine sender; TransferRequest send;
    send.direction=Direction::Send; send.files={{"file",10}};
    send.config.maxRetries=2;
    sender.start(send,0);
    const auto first=sender.pendingOutput();
    Bytes packet(first.data,first.data+first.size);
    sender.advance(4000);
    check(sender.progress().retransmissions==0,"no retry before full drain");
    sender.acknowledgeOutput(1,4000); sender.advance(8000);
    check(sender.progress().retransmissions==0,"partial output does not start deadline");
    sender.acknowledgeOutput(static_cast<std::size_t>(sender.pendingOutput().size),8000);
    sender.advance(11000);
    check(drain(sender)==packet && sender.progress().retransmissions==1,"handshake timed retry");
    sender.advance(14000); drain(sender); sender.advance(17000);
    check(sender.progress().error==Error::RetryLimit,"retry budget finite");
    for(unsigned iteration=0;iteration<100;++iteration) {
        ZmodemEngine engine; TransferRequest receive;
        check(engine.start(receive,0),"lifecycle start");
        check(engine.pendingOutput().size<=static_cast<NovaTerm::isize>(MaxOutputBytes),"output budget");
        drain(engine); offer(engine,Bytes{'x',0,'1',0});
        const auto pending=response(engine);
        check(pending.position==0,"new receive zero offset");
        feed(engine,ZmodemCodec::header(ZFrame::Data,0,ZHeaderFormat::Binary16));
        feed(engine,ZmodemCodec::data(byteView(Bytes{1}),ZEnd::Wait,false));
        const auto action=engine.takeAction(); check(action.has_value(),"lifecycle pending action");
        engine.cancel(0);
        check(!engine.completeOperation(action->id,{},0),"late lifecycle result ignored");
    }
    ZmodemCodec codec; codec.expectData(false,1);
    check(!codec.feed('a') && codec.feed('b')->kind==ZEventKind::Error,"payload limit bounded");
}
void batchTests(bool crc32)
{
    ZmodemEngine sender,receiver;
    TransferRequest send,receive;
    send.direction=Direction::Send; send.config.zmodemCrc32=crc32;
    receive.config.zmodemCrc32=crc32;
    Bytes source;
    for(unsigned i=0;i<33001;++i) source.push_back(static_cast<std::uint8_t>(i));
    send.files={{"binary.dat",source.size()},{"empty",0}};
    check(sender.start(send,0) && receiver.start(receive,0),"batch start");
    Bytes forward,backward,target;
    unsigned writes=0;
    for(unsigned iteration=0;iteration<200000;++iteration) {
        for(auto* engine:{&sender,&receiver}) {
            if(auto action=engine->takeAction()) {
                OperationResult result;
                if(action->kind==ActionKind::ReadAt) {
                    check(action->offset+action->count<=source.size(),"bounded reads");
                    result.bytes.assign(source.begin()+static_cast<std::ptrdiff_t>(action->offset),
                        source.begin()+static_cast<std::ptrdiff_t>(action->offset+action->count));
                } else if(action->kind==ActionKind::WriteAt) {
                    check(action->offset==target.size(),"contiguous writes");
                    target.insert(target.end(),action->bytes.begin(),action->bytes.end()); ++writes;
                }
                check(engine->completeOperation(action->id,std::move(result),0),"operation completion");
            }
        }
        auto append=[](Bytes& into,Bytes bytes){into.insert(into.end(),bytes.begin(),bytes.end());};
        append(forward,drain(sender)); append(backward,drain(receiver));
        for(auto pair:{std::pair<ITransferEngine*,Bytes*>{&receiver,&forward},
                       std::pair<ITransferEngine*,Bytes*>{&sender,&backward}}) {
            if(!pair.second->empty()) {
                const auto n=pair.first->consume({reinterpret_cast<const char*>(pair.second->data()),1},0).consumed;
                pair.second->erase(pair.second->begin(),pair.second->begin()+static_cast<std::ptrdiff_t>(n));
            }
        }
        check(sender.progress().state!=State::Failed && receiver.progress().state!=State::Failed,
              "batch protocol failure");
        if(terminal(sender.progress().state) && terminal(receiver.progress().state)) break;
    }
    check(sender.progress().state==State::Completed && receiver.progress().state==State::Completed,
          "batch completion");
    check(target==source && writes>1,"batch exact bytes");
    check(sender.progress().completedFiles==2 && receiver.progress().completedFiles==2,
          "empty file included");
    check(sender.progress().transferredBytes==source.size(),"sender confirmed count");
}
}
int main()
{ codecTests(); committedOffsetTests(); errorTests(); duplicateMetadataBudgetTests(); metadataTests();
  senderFeedbackTests(); closingTests(); metadataBudgetTests();
  challengeRetryTests(); timingAndBudgetTests(); batchTests(false); batchTests(true);
  std::cout << "ZMODEM tests passed\n"; }
