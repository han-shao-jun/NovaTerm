/** @file ZmodemEngine.h @brief 非阻塞、单操作、有限窗口 ZMODEM 双向引擎。 */
#pragma once
#include "EngineSupport.h"
#include "ZmodemCodec.h"
namespace NovaTerm::FileTransfer {
/** @brief 支持 CRC16/32 固定头与标准 batch；不执行命令或跨任务续传。 */
class ZmodemEngine final : public EngineSupport {
protected:
    bool onStart(TimePoint now) override;
    void onByte(std::uint8_t byte,TimePoint now) override;
    void onTimeout(TimePoint now) override;
    void onOperation(const TransferAction& action,OperationResult result,
                     TimePoint now) override;
    void onOutputDrained(TimePoint now) override;
private:
    enum class Phase { ReceiveHeader, FileMetadata, InitData, ReceiveData,
                       WaitOO, WaitInit, WaitPosition, Reading, WaitAck,
                       WaitEofAck, WaitFinish };
    void event(ZEvent item,TimePoint now);
    void receivedHeader(const ZEvent& item,TimePoint now);
    void sentHeader(const ZEvent& item,TimePoint now);
    void receivedData(ZEvent item,TimePoint now);
    void sendInit(TimePoint now);
    void sendFile(TimePoint now);
    void readNext(TimePoint now);
    void sendEof(TimePoint now);
    void sendResume(TimePoint now);
    void acknowledgePosition(std::uint64_t position);
    void sendPacket(Bytes bytes,TimePoint now);
    void fileAction(ActionKind kind,TimePoint now);
    [[nodiscard]] ZHeaderFormat binaryFormat() const;
    ZmodemCodec _codec;
    Phase _phase{Phase::ReceiveHeader};
    Bytes _lastPacket;
    FileInfo _file;
    std::uint64_t _offset{0};
    std::uint64_t _sentHighWater{0};
    std::uint64_t _fileConfirmed{0};
    std::size_t _metadataBytes{0};
    std::size_t _chunkSize{1024};
    bool _crc32{true};
    bool _receiveActive{false};
    bool _dataCrc32{false};
    bool _awaitDrain{false};
    unsigned _ooCount{0};
    std::optional<TimePoint> _closingEnd;
    ZEnd _receivedEnd{ZEnd::End};
};
} // namespace NovaTerm::FileTransfer
