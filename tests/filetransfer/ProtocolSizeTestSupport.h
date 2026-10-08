/**
 * @file ProtocolSizeTestSupport.h
 * @brief 三种协议的流式文件大小、分块与序号回绕验收夹具。
 */
#pragma once
#include "filetransfer/XmodemEngine.h"
#include "filetransfer/YmodemEngine.h"
#include "filetransfer/ZmodemEngine.h"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ProtocolSizeTests {
namespace FT = NovaTerm::FileTransfer;
enum class Mode { XChecksum, XCrc, XOneK, Y, Z16, Z32 };

inline void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
inline std::unique_ptr<FT::ITransferEngine> engine(Mode mode)
{
    if (mode == Mode::Y) return std::make_unique<FT::YmodemEngine>();
    if (mode == Mode::Z16 || mode == Mode::Z32)
        return std::make_unique<FT::ZmodemEngine>();
    return std::make_unique<FT::XmodemEngine>();
}
inline bool isX(Mode mode) { return mode <= Mode::XOneK; }
inline FT::TransferConfig config(Mode mode)
{
    FT::TransferConfig result;
    if (mode == Mode::XChecksum) result.xmodemMode = FT::XmodemMode::Checksum;
    if (mode == Mode::XOneK) result.xmodemMode = FT::XmodemMode::OneK;
    result.zmodemCrc32 = mode != Mode::Z16;
    return result;
}
inline const char* name(Mode mode)
{
    switch (mode) {
    case Mode::XChecksum: return "XMODEM-checksum";
    case Mode::XCrc: return "XMODEM-CRC";
    case Mode::XOneK: return "XMODEM-1K";
    case Mode::Y: return "YMODEM";
    case Mode::Z16: return "ZMODEM-CRC16";
    case Mode::Z32: return "ZMODEM-CRC32";
    }
    throw std::runtime_error("unknown test mode");
}
/** @brief 独立按位置生成载荷；真实文件尾含 1A/00，不能当作填充裁剪。 */
inline std::uint8_t sourceByte(std::uint64_t offset, std::uint64_t size)
{
    if (offset + 1 == size) return 0;
    if (offset + 2 == size) return 0x1a;
    return static_cast<std::uint8_t>((offset * 73U + offset / 251U) & 0xffU);
}
inline void roundTrip(Mode mode, std::uint64_t size, bool knownLength = true)
{
    auto sender = engine(mode), receiver = engine(mode);
    FT::TransferRequest send, receive;
    send.direction = FT::Direction::Send;
    send.files = {{"size-case.bin", size}};
    send.config = receive.config = config(mode);
    if (isX(mode) && knownLength) receive.expectedSize = size;
    require(sender->start(send, 0) && receiver->start(receive, 0), "size-case start");
    const auto block = mode == Mode::XOneK ? 1024ULL : 128ULL;
    const auto expectedReceived = isX(mode) && !knownLength
        ? ((size + block - 1) / block) * block : size;
    FT::Bytes forward, backward;
    std::uint64_t readOffset = 0, writeOffset = 0;
    unsigned sentFinishes = 0, receivedFinishes = 0;
    std::size_t partialDrains = 0;
    FT::TimePoint now = 0;
    const auto maxRounds = 20000ULL + (size / 128 + 1) * 32;
    for (std::uint64_t round = 0; round < maxRounds; ++round) {
        bool moved = false;
        for (const bool sending : {true, false}) {
            auto& current = sending ? *sender : *receiver;
            if (auto action = current.takeAction()) {
                FT::OperationResult result;
                if (action->kind == FT::ActionKind::ReadAt) {
                    require(sending && action->offset == readOffset && action->count > 0
                            && action->offset <= size && action->count <= size - action->offset,
                            "size-case contiguous bounded source read");
                    result.bytes.resize(action->count);
                    for (std::size_t i = 0; i < action->count; ++i)
                        result.bytes[i] = sourceByte(action->offset + i, size);
                    readOffset += action->count;
                } else if (action->kind == FT::ActionKind::WriteAt) {
                    require(!sending && action->offset == writeOffset && !action->bytes.empty()
                            && action->offset <= expectedReceived
                            && action->bytes.size() <= expectedReceived - action->offset,
                            "size-case contiguous bounded target write");
                    for (std::size_t i = 0; i < action->bytes.size(); ++i) {
                        const auto offset = action->offset + i;
                        const auto expected = offset < size ? sourceByte(offset, size) : 0x1a;
                        require(action->bytes[i] == expected, "size-case exact payload/padding");
                    }
                    writeOffset += action->bytes.size();
                } else if (action->kind == FT::ActionKind::OfferFile) {
                    require(!sending, "size-case offer only at receiver");
                    if (!isX(mode)) require(action->file.size == size, "size-case metadata length");
                } else if (action->kind == FT::ActionKind::FinishFile) {
                    if (sending) { require(readOffset == size, "size-case source finished early"); ++sentFinishes; }
                    else { require(writeOffset == expectedReceived, "size-case target finished early"); ++receivedFinishes; }
                }
                require(current.completeOperation(action->id, std::move(result), now),
                        "size-case operation completion");
                moved = true;
            }
        }
        // 每个方向最多暂存 32 KiB；短写、分片和文件等待均经过真实引擎。
        for (const bool sending : {true, false}) {
            auto& current = sending ? *sender : *receiver;
            auto& wire = sending ? forward : backward;
            const auto output = current.pendingOutput();
            require(output.size <= static_cast<NovaTerm::isize>(FT::MaxOutputBytes),
                    "size-case output exceeds protocol budget");
            const auto count = std::min<std::size_t>({static_cast<std::size_t>(output.size),
                                                     97, 32768 - wire.size()});
            if (count) {
                if (count < static_cast<std::size_t>(output.size)) ++partialDrains;
                wire.insert(wire.end(), output.data, output.data + count);
                require(current.acknowledgeOutput(count, now), "size-case partial output drain");
                moved = true;
            }
        }
        for (const bool sending : {true, false}) {
            auto& current = sending ? *receiver : *sender;
            auto& wire = sending ? forward : backward;
            if (!wire.empty()) {
                const auto count = std::min<std::size_t>(wire.size(), round % 2 ? 4091 : 17);
                const auto consumed = current.consume(
                    {reinterpret_cast<const char*>(wire.data()), static_cast<NovaTerm::isize>(count)}, now).consumed;
                require(consumed <= count, "size-case invalid consume count");
                wire.erase(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(consumed));
                moved = moved || consumed > 0;
            }
        }
        require(sender->progress().state != FT::State::Failed
                && receiver->progress().state != FT::State::Failed,
                "size-case protocol failure");
        if (FT::terminal(sender->progress().state) && FT::terminal(receiver->progress().state)) break;
        if (!moved) {
            require(sender->progress().state == FT::State::Completed
                    && receiver->progress().state == FT::State::Closing,
                    "size-case stalled before completion");
            const auto deadline = receiver->nextDeadline();
            require(deadline.has_value(), "size-case closing without deadline");
            now = *deadline;
            receiver->advance(now);
        }
    }
    require(sender->progress().state == FT::State::Completed
            && receiver->progress().state == FT::State::Completed, "size-case incomplete handshake");
    require(readOffset == size && writeOffset == expectedReceived, "size-case final byte counts");
    require(sentFinishes == 1 && receivedFinishes == 1, "size-case commit exactly once");
    require(sender->progress().completedFiles == 1 && receiver->progress().completedFiles == 1,
            "size-case completed file count");
    require(sender->progress().transferredBytes == size
            && receiver->progress().transferredBytes == expectedReceived, "size-case final progress");
    // YMODEM 的标准 EOT/NAK/EOT 握手会记一次重发，不是数据重传。
    require(sender->progress().retransmissions == (mode == Mode::Y ? 1U : 0U)
            && receiver->progress().retransmissions == 0,
            "size-case unexpected retransmission");
    require(forward.empty() && backward.empty(), "size-case unconsumed protocol bytes");
    if (size >= 1024) require(partialDrains > 0, "size-case must exercise partial output drain");
}
inline void lengthLimits(Mode mode)
{
    // 只检查长度授权/声明，不分配或声称真实传完 4 GiB 文件。
    for (const auto size : {0xfffffffeULL, 0xffffffffULL, 0x100000000ULL}) {
        auto sender = engine(mode), receiver = engine(mode);
        FT::TransferRequest send, receive;
        send.direction = FT::Direction::Send;
        send.config = receive.config = config(mode);
        send.files = {{"limit.bin", size}};
        receive.expectedSize = size;
        const bool accepted = size <= 0xffffffffULL;
        require(sender->start(send, 0) == accepted && receiver->start(receive, 0) == accepted,
                "size-case 32-bit length limit");
        if (!accepted)
            require(sender->progress().error == FT::Error::InvalidRequest
                    && receiver->progress().error == FT::Error::InvalidRequest,
                    "size-case oversized length rejection");
    }
}
inline void matrix(Mode mode)
{
    const std::uint64_t block = mode == Mode::XChecksum || mode == Mode::XCrc ? 128 : 1024;
    std::vector<std::uint64_t> boundaries{0, 1, 127, 128, 129, 1023, 1024, 1025,
                                          8191, 8192, 8193};
    if (isX(mode) || mode == Mode::Y)
        for (const auto packets : {255ULL, 256ULL, 257ULL})
            for (const auto delta : {-1, 0, 1})
                boundaries.push_back(packets * block - 1 + static_cast<unsigned>(delta + 1));
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    for (const auto size : boundaries) {
        try { roundTrip(mode, size); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(name(mode)) + " boundary size="
                                     + std::to_string(size) + ": " + error.what());
        }
    }
    if (isX(mode)) for (const auto size : {1ULL, 127ULL, 128ULL, 129ULL, 1023ULL, 1024ULL, 1025ULL})
        roundTrip(mode, size, false);
    lengthLimits(mode);
    std::cout << "PASS " << name(mode) << " boundary cases=" << boundaries.size() << '\n';
    for (const auto size : {64ULL * 1024, 1024ULL * 1024, 5ULL * 1024 * 1024, 10ULL * 1024 * 1024}) {
        try { roundTrip(mode, size); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(name(mode)) + " gradient size="
                                     + std::to_string(size) + ": " + error.what());
        }
        std::cout << "PASS " << name(mode) << " gradient size=" << size << '\n';
    }
}
} // namespace ProtocolSizeTests
