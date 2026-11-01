/** @file Checksum.cpp @brief CRC16/XMODEM 与反射 CRC32 的实现。 */
#include "Checksum.h"
#include <array>
namespace NovaTerm::FileTransfer {
namespace {
constexpr auto makeCrc16Table() {
    std::array<std::uint16_t, 256> table{};
    for (unsigned index = 0; index < table.size(); ++index) {
        auto value = std::uint16_t(index << 8U);
        for (unsigned bit = 0; bit < 8; ++bit)
            value = std::uint16_t((unsigned(value) << 1U) ^ ((value & 0x8000U) ? 0x1021U : 0U));
        table[index] = value;
    }
    return table;
}
constexpr auto makeCrc32Table() {
    std::array<std::uint32_t, 256> table{};
    for (unsigned index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
        table[index] = value;
    }
    return table;
}
constexpr auto Crc16Table = makeCrc16Table();
constexpr auto Crc32Table = makeCrc32Table();
}
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
    return std::uint16_t((unsigned(crc) << 8U) ^ Crc16Table[(unsigned(crc) >> 8U) ^ byte]);
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
    return (crc >> 8U) ^ Crc32Table[(crc ^ byte) & 0xffU];
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
