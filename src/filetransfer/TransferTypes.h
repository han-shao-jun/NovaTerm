/**
 * @file TransferTypes.h
 * @brief 独立文件传输协议的值类型与资源预算。
 */
#pragma once
#include "../core/CoreTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NovaTerm::FileTransfer {
using Bytes = std::vector<std::uint8_t>;
using TimePoint = std::uint64_t; ///< 宿主注入的单调毫秒，不是墙上时钟。
using OperationId = std::uint64_t;
inline constexpr std::size_t MaxInputBytes = 64 * 1024;
inline constexpr std::size_t MaxOutputBytes = 64 * 1024;
inline constexpr std::uint64_t MaxFileSize = 0xffffffffULL;
inline constexpr std::size_t MaxFiles = 256;

enum class Direction { Send, Receive };
enum class XmodemMode { Checksum, Crc, OneK };
enum class State { Idle, Negotiating, Transferring, Finishing, Closing,
                   Completed, Cancelled, Failed };
enum class Error { None, InvalidRequest, Protocol, UnsupportedVariant,
                   RetryLimit, Timeout, FileIo, SizeMismatch, ResourceLimit };
enum class ActionKind { OfferFile, ReadAt, WriteAt, FinishFile };

struct FileInfo {
    std::string name;
    std::optional<std::uint64_t> size;
};
struct TransferConfig {
    XmodemMode xmodemMode{XmodemMode::Crc};
    TimePoint handshakeTimeoutMs{30000};
    TimePoint handshakeRetryMs{3000};
    TimePoint responseTimeoutMs{10000};
    TimePoint operationTimeoutMs{30000};
    TimePoint closingTimeoutMs{3000};
    unsigned maxRetries{10};
    bool zmodemCrc32{true};
};
struct TransferRequest {
    Direction direction{Direction::Receive};
    std::vector<FileInfo> files; ///< 发送源的元信息；发送任务必须提供真实长度。
    std::string receiveName{"received.bin"}; ///< XMODEM 的宿主目标标识。
    std::optional<std::uint64_t> expectedSize;
    TransferConfig config;
};
struct TransferAction {
    OperationId id{0};
    ActionKind kind{ActionKind::OfferFile};
    std::size_t fileIndex{0};
    FileInfo file;
    std::uint64_t offset{0};
    std::size_t count{0};
    Bytes bytes;
};
struct OperationResult {
    bool success{true}; ///< WriteAt/FinishFile 的成功表示该操作整体完成。
    bool accepted{true}; ///< OfferFile 的接受决定。
    Bytes bytes; ///< ReadAt 的数据；短读由引擎根据请求长度判错。
};
struct ConsumeResult {
    std::size_t consumed{0};
    bool waiting{false};
};
struct Progress {
    State state{State::Idle};
    Error error{Error::None};
    std::size_t fileIndex{0};
    std::size_t completedFiles{0};
    std::uint64_t transferredBytes{0}; ///< 连续有效数据，重传不重复累计。
    std::uint64_t retransmissions{0};
    bool lengthKnown{false};
};
/** @brief 验证 UTF-8/ASCII 文件标识和 255 字节上限；路径授权由宿主处理。 */
[[nodiscard]] bool validFileNameUtf8(std::string_view name) noexcept;
/** @brief 查询不再接收协议数据的状态；Closing 仍需要驱动结束重试。 */
[[nodiscard]] inline bool terminal(State state) noexcept
{
    return state == State::Completed || state == State::Cancelled
        || state == State::Failed;
}
/** @brief 把字节向量映射为只读视图；生命周期由向量拥有者保证。 */
[[nodiscard]] inline ByteView byteView(const Bytes& bytes) noexcept
{
    return {reinterpret_cast<const char*>(bytes.data()),
            static_cast<isize>(bytes.size())};
}
} // namespace NovaTerm::FileTransfer
