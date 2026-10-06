/** @file XyPacketCodec.h @brief X/YMODEM 有界数据包编解码器。 */
#pragma once
#include "TransferTypes.h"
#include <array>
namespace NovaTerm::FileTransfer {
/** @brief 跨分片保存一个包；控制字节仅在包外由引擎解释。 */
class XyPacketCodec {
public:
    enum class Result { Idle, Partial, Valid, Invalid };
    struct Packet { std::uint8_t number{0}; Bytes bytes; };
    /** @brief 消费一个字节；每包首次输入时冻结校验方式。 */
    Result push(std::uint8_t byte, bool useCrc);
    void reset() noexcept;
    [[nodiscard]] bool receiving() const noexcept { return _used != 0; }
    [[nodiscard]] const Packet& packet() const noexcept { return _packet; }
    /** @brief CRC 包可为 128/1024 字节；checksum 仅 128 字节，非法时返回空。 */
    [[nodiscard]] static Bytes encode(std::uint8_t number, const Bytes& payload,
                                      bool useCrc);
private:
    std::array<std::uint8_t, 1029> _frame{};
    std::size_t _used{0};
    std::size_t _required{0};
    bool _useCrc{true};
    Packet _packet;
};
} // namespace NovaTerm::FileTransfer
