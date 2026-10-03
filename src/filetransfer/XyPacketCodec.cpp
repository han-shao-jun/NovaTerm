/** @file XyPacketCodec.cpp @brief 包号反码、checksum 与大端 CRC16 校验。 */
#include "XyPacketCodec.h"
#include "Checksum.h"
namespace NovaTerm::FileTransfer {
void XyPacketCodec::reset() noexcept { _used = 0; _required = 0; }
XyPacketCodec::Result XyPacketCodec::push(std::uint8_t byte, bool useCrc)
{
    if (!_used) {
        if (byte != 1 && byte != 2) return Result::Idle;
        _useCrc = useCrc;
        _required = (byte == 1 ? 128U : 1024U) + 3U + (_useCrc ? 2U : 1U);
    }
    _frame[_used++] = byte;
    if (_used != _required) return Result::Partial;
    const auto payloadSize = _required - 3U - (_useCrc ? 2U : 1U);
    const ByteView data{reinterpret_cast<const char*>(_frame.data() + 3),
                        static_cast<isize>(payloadSize)};
    bool valid = static_cast<unsigned>(_frame[1]) + _frame[2] == 255U
        && (_useCrc || _frame[0] == 1);
    if (_useCrc) {
        const auto expected = static_cast<std::uint16_t>(
            (static_cast<unsigned>(_frame[3 + payloadSize]) << 8U)
            | _frame[4 + payloadSize]);
        valid = valid && crc16(data) == expected;
    } else {
        valid = valid && checksum8(data) == _frame[3 + payloadSize];
    }
    if (valid) {
        _packet.number = _frame[1];
        _packet.bytes.assign(_frame.begin() + 3,
                              _frame.begin() + static_cast<std::ptrdiff_t>(3 + payloadSize));
    }
    reset();
    return valid ? Result::Valid : Result::Invalid;
}
Bytes XyPacketCodec::encode(std::uint8_t number, const Bytes& payload, bool useCrc)
{
    if ((payload.size() != 128 && payload.size() != 1024)
        || (!useCrc && payload.size() != 128)) return {};
    Bytes frame;
    frame.reserve(payload.size() + 5);
    frame.push_back(payload.size() == 128 ? 1 : 2);
    frame.push_back(number);
    frame.push_back(static_cast<std::uint8_t>(~number));
    frame.insert(frame.end(), payload.begin(), payload.end());
    if (useCrc) {
        const auto crc = crc16(byteView(payload));
        frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
        frame.push_back(static_cast<std::uint8_t>(crc));
    } else {
        frame.push_back(checksum8(byteView(payload)));
    }
    return frame;
}
} // namespace NovaTerm::FileTransfer
