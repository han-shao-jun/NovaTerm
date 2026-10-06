/** @file EngineSupportTests.cpp @brief 部分写、任务世代与宿主等待的契约测试。 */
#include "filetransfer/EngineSupport.h"
#include <iostream>
using namespace NovaTerm::FileTransfer;
namespace {
class Probe final : public EngineSupport {
public:
    explicit Probe(bool negotiating = false) : _negotiating(negotiating) {}
private:
    bool _negotiating;
    bool onStart(TimePoint now) override
    {
        _progress.state = _negotiating ? State::Negotiating : State::Transferring;
        emitBytes(Bytes{1,2,3,4});
        setDeadline(now, 10);
        return true;
    }
    void onByte(std::uint8_t, TimePoint now) override
    {
        TransferAction action; action.kind = ActionKind::WriteAt;
        action.bytes = {1}; requestAction(std::move(action), now);
    }
    void onTimeout(TimePoint) override { fail(Error::Timeout); }
    void onOperation(const TransferAction&, OperationResult, TimePoint) override
    { _progress.transferredBytes++; }
};
}
int main()
{
    Probe p;
    TransferRequest req;
    if (!p.start(req, 0) || p.acknowledgeOutput(5,0)
        || !p.acknowledgeOutput(2,0) || p.pendingOutput().size != 2
        || static_cast<unsigned char>(p.pendingOutput().data[0]) != 3) return 1;
    const auto consumed = p.consume({"abc",3},0);
    if (consumed.consumed != 1 || !consumed.waiting) return 2;
    const auto action = p.takeAction();
    if (!action || p.takeAction() || p.completeOperation(action->id+1,{},0)) return 3;
    p.cancel(0);
    if (p.completeOperation(action->id,{},0) || !p.start(req,1)) return 4;
    p.consume({"a",1},1);
    const auto newAction = p.takeAction();
    if (!newAction || newAction->id <= action->id
        || !p.completeOperation(newAction->id,{},1)
        || p.progress().transferredBytes != 1) return 5;
    p.advance(11);
    if (p.progress().error != Error::Timeout) return 6;
    req.expectedSize = 0x100000000ULL;
    if (p.start(req,20) || p.progress().error != Error::InvalidRequest) return 7;
    req.expectedSize.reset();
    if (!p.start(req,30)) return 8;
    p.consume({"a",1},30);
    const auto stalled = p.takeAction();
    p.advance(30031);
    if (p.progress().error != Error::FileIo
        || p.completeOperation(stalled->id,{},30031)) return 9;
    req.files = {{std::string(1, static_cast<char>(0xff)), 0}};
    if (p.start(req,40000) || p.progress().error != Error::InvalidRequest) return 10;
    Probe negotiating(true);
    req.files.clear(); req.config.handshakeTimeoutMs = 5;
    if (!negotiating.start(req, 0)) return 11;
    negotiating.consume({"a",1},0);
    const auto offer = negotiating.takeAction();
    negotiating.advance(5);
    if (!offer || negotiating.progress().error != Error::Timeout
        || negotiating.completeOperation(offer->id,{},5)) return 12;
    Probe late;
    req.config.handshakeTimeoutMs = 30000;
    if (!late.start(req, 0)) return 13;
    late.consume({"a",1},0);
    const auto lateAction = late.takeAction();
    if (!lateAction || late.completeOperation(lateAction->id, {}, 30001)
        || late.progress().error != Error::FileIo) return 14;
    std::cout << "EngineSupport contracts PASS\n";
    return 0;
}
