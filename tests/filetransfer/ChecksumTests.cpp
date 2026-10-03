/** @file ChecksumTests.cpp @brief 校验算法的独立已知向量测试。 */
#include "filetransfer/Checksum.h"
#include <iostream>
int main()
{
    const NovaTerm::ByteView data{"123456789", 9};
    if (NovaTerm::FileTransfer::crc16(data) != 0x31c3
        || NovaTerm::FileTransfer::crc32(data) != 0xcbf43926U
        || NovaTerm::FileTransfer::checksum8(data) != 0xdd) {
        std::cerr << "checksum vector mismatch\n";
        return 1;
    }
    return 0;
}
