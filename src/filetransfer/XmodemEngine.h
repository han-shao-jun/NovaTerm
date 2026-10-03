/** @file XmodemEngine.h @brief XMODEM 单文件双向事件状态机。 */
#pragma once
#include "EngineSupport.h"
#include "XyPacketCodec.h"
namespace NovaTerm::FileTransfer {
class XmodemEngine final : public EngineSupport {
public:
    XmodemEngine() = default;
protected:
    bool onStart(TimePoint now) override;
    void onByte(std::uint8_t byte, TimePoint now) override;
    void onTimeout(TimePoint now) override;
    void onOperation(const TransferAction& action, OperationResult result,
                     TimePoint now) override;
    void onOutputDrained(TimePoint now) override;
private:
    enum class Phase { Handshake, Data, Eot, Closing };
    void sendNext(TimePoint now);
    void sendControl(std::uint8_t byte);
    void resend(TimePoint now);
    void receivePacket(TimePoint now);
    void finishReceive(TimePoint now);
    XyPacketCodec _codec;
    Bytes _lastFrame;
    Phase _phase{Phase::Handshake};
    std::uint64_t _offset{0};
    std::size_t _pendingCount{0};
    std::size_t _blockSize{128};
    std::uint8_t _block{1};
    bool _useCrc{true};
    bool _canSeen{false};
    bool _haveBlock{false};
};
} // namespace NovaTerm::FileTransfer
