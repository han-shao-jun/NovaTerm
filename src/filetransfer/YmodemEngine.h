/** @file YmodemEngine.h @brief 标准 CRC YMODEM 批次双向状态机。 */
#pragma once
#include "EngineSupport.h"
#include "XyPacketCodec.h"
namespace NovaTerm::FileTransfer {
class YmodemEngine final : public EngineSupport {
public:
    YmodemEngine() = default;
protected:
    bool onStart(TimePoint now) override;
    void onByte(std::uint8_t byte, TimePoint now) override;
    void onTimeout(TimePoint now) override;
    void onOperation(const TransferAction& action, OperationResult result,
                     TimePoint now) override;
    void onOutputDrained(TimePoint now) override;
private:
    enum class Phase { Header, HeaderAck, WaitData, Data, Eot, NextHeader,
                       EndAck, Closing };
    void sendHeader();
    void sendNext(TimePoint now);
    void sendControl(const Bytes& bytes);
    void resend();
    void receivePacket(TimePoint now);
    void receiveHeader(const XyPacketCodec::Packet& packet, TimePoint now);
    void receiveEot(TimePoint now);
    [[nodiscard]] bool parseHeader(const Bytes& bytes, FileInfo& file) const;
    XyPacketCodec _codec;
    Bytes _lastFrame;
    Bytes _lastHeader;
    FileInfo _file;
    Phase _phase{Phase::Header};
    std::size_t _fileIndex{0};
    std::size_t _metadataBytes{0};
    std::uint64_t _offset{0};
    std::size_t _pendingCount{0};
    std::size_t _blockSize{1024};
    std::uint8_t _block{1};
    bool _canSeen{false};
    bool _haveBlock{false};
    bool _finishedFile{false};
};
} // namespace NovaTerm::FileTransfer
