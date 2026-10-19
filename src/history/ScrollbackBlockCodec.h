/** @file ScrollbackBlockCodec.h
 * @brief 无 Qt 的无损历史块编码；LZ4 细节仅存在于实现文件。
 */
#pragma once
#include "core/scrollback/ScrollbackData.h"
#include <string>

namespace NovaTerm {
/** @brief 编解码分配硬上限；验证完成前不分配 Cell。 */
struct HistoryCodecLimits {
    std::size_t decodedBytes{1024 * 1024};
    std::size_t serializedBytes{1024 * 1024};
    std::size_t lines{1024};
};
enum class HistoryEncoding : u8 { Raw = 0, Lz4 = 1 };
/** @brief 只读后发布；索引查询不解压。不作为持久化文件格式。 */
struct EncodedHistoryBlock {
    u32 version{1};
    HistoryEncoding encoding{HistoryEncoding::Lz4};
    u64 serializedBytes{0};
    u64 payloadBytes{0};
    u64 decodedBytes{0};
    u64 originalCharge{0};
    std::vector<HistoryLineIndex> index;
    std::vector<char> payload;
    [[nodiscard]] std::size_t byteSize() const;
};
struct HistoryEncodeResult {
    EncodedHistoryBlock block;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
struct HistoryDecodeResult {
    std::vector<LogicalLine> lines;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
class ScrollbackBlockCodec {
public:
    /** @brief 按固定字段、小端编码，不复制结构体 padding。 */
    static HistoryEncodeResult encode(const std::vector<LogicalLine>& lines,
                                     HistoryCodecLimits limits = {});
    /** @brief 校验版本/索引/分配量/枚举/长度；损坏返回 error，不返回假空行。 */
    static HistoryDecodeResult decode(const EncodedHistoryBlock& block,
                                     HistoryCodecLimits limits = {});
    /** @brief 结构化解码结果的保守分配估算，包含容量、对齐和分配器开销。 */
    static std::size_t decodedCharge(const std::vector<HistoryLineIndex>& index);
};
} // namespace NovaTerm
