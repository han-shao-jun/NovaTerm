/** @file ChecksumTests.cpp @brief 校验算法的独立已知向量测试。 */
#include "filetransfer/Checksum.h"
#include <iostream>
namespace {
std::uint16_t reference16(std::uint16_t crc, std::uint8_t byte) {
    crc ^= std::uint16_t(unsigned(byte) << 8U);
    for (unsigned bit=0; bit<8; ++bit)
        crc = std::uint16_t((unsigned(crc) << 1U) ^ ((crc & 0x8000U) ? 0x1021U : 0U));
    return crc;
}
std::uint32_t reference32(std::uint32_t crc, std::uint8_t byte) {
    crc ^= byte;
    for (unsigned bit=0; bit<8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    return crc;
}
}
int main()
{
    const NovaTerm::ByteView data{"123456789", 9};
    if (NovaTerm::FileTransfer::crc16(data) != 0x31c3
        || NovaTerm::FileTransfer::crc32(data) != 0xcbf43926U
        || NovaTerm::FileTransfer::checksum8(data) != 0xdd) {
        std::cerr << "checksum vector mismatch\n";
        return 1;
    }
    // Exhaust every CRC16 state/input transition against the bitwise definition.
    for (unsigned state=0; state<65536; ++state) {
        for (unsigned byte=0; byte<256; ++byte) {
            if (NovaTerm::FileTransfer::crc16Update(std::uint16_t(state), std::uint8_t(byte))
                != reference16(std::uint16_t(state), std::uint8_t(byte))) return 2;
        }
    }
    // CRC32 uses the same byte transition; cover all bytes over varied full states.
    std::uint32_t state = 0xffffffffU;
    for (unsigned sample=0; sample<4096; ++sample) {
        state = state * 1664525U + 1013904223U;
        for (unsigned byte=0; byte<256; ++byte) {
            if (NovaTerm::FileTransfer::crc32Update(state, std::uint8_t(byte))
                != reference32(state, std::uint8_t(byte))) return 3;
        }
    }
    return 0;
}
