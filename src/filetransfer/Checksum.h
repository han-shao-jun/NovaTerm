/** @file Checksum.h @brief X/Y/ZMODEM 的校验基础更新函数。 */
#pragma once
#include "TransferTypes.h"
namespace NovaTerm::FileTransfer {
[[nodiscard]] std::uint8_t checksum8(ByteView data) noexcept;
[[nodiscard]] std::uint16_t crc16Update(std::uint16_t crc, std::uint8_t byte) noexcept;
[[nodiscard]] std::uint16_t crc16(ByteView data) noexcept;
[[nodiscard]] std::uint32_t crc32Update(std::uint32_t crc, std::uint8_t byte) noexcept;
[[nodiscard]] std::uint32_t crc32(ByteView data) noexcept;
} // namespace NovaTerm::FileTransfer
