/** @file Checksum.cpp @brief CRC16/XMODEM 与反射 CRC32 的实现。 */
#include "Checksum.h"
namespace NovaTerm::FileTransfer {
std::uint8_t checksum8(ByteView data) noexcept
{
    std::uint8_t sum = 0;
    if (!data.data) return sum;
    for (isize i = 0; i < data.size; ++i)
        sum = static_cast<std::uint8_t>(sum + static_cast<std::uint8_t>(data.data[i]));
    return sum;
}
std::uint16_t crc16Update(std::uint16_t crc, std::uint8_t byte) noexcept
{
    crc ^= static_cast<std::uint16_t>(static_cast<unsigned>(byte) << 8U);
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = static_cast<std::uint16_t>((static_cast<unsigned>(crc) << 1U)
              ^ ((crc & 0x8000U) ? 0x1021U : 0U));
    return crc;
}
std::uint16_t crc16(ByteView data) noexcept
{
    std::uint16_t crc = 0;
    if (!data.data) return crc;
    for (isize i = 0; i < data.size; ++i)
        crc = crc16Update(crc, static_cast<std::uint8_t>(data.data[i]));
    return crc;
}
std::uint32_t crc32Update(std::uint32_t crc, std::uint8_t byte) noexcept
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    return crc;
}
std::uint32_t crc32(ByteView data) noexcept
{
    std::uint32_t crc = 0xffffffffU;
    if (data.data)
        for (isize i = 0; i < data.size; ++i)
            crc = crc32Update(crc, static_cast<std::uint8_t>(data.data[i]));
    return ~crc;
}
} // namespace NovaTerm::FileTransfer
