/** @file ScrollbackBlockCodec.cpp
 * @brief 显式字段序列化和经过限额校验的 LZ4 基础块编解码。
 */
#include "ScrollbackBlockCodec.h"
#include <lz4.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace NovaTerm {
namespace {
constexpr u64 CellBytes = 38;
constexpr u32 AttributeMask = (1u << 14) - 1;
void put(std::vector<char>& bytes, u64 value, unsigned count) {
    for (unsigned n = 0; n < count; ++n) bytes.push_back(char((value >> (n * 8)) & 255));
}
u64 get(const std::vector<char>& bytes, std::size_t& offset, unsigned count) {
    u64 value = 0;
    for (unsigned n = 0; n < count; ++n) value |= u64(static_cast<u8>(bytes.at(offset++))) << (n * 8);
    return value;
}
u32 attributes(const CellAttributes& a) {
    return u32(a.bold) | (u32(a.underline) << 1) | (u32(a.italic) << 2)
        | (u32(a.blink) << 3) | (u32(a.reverse) << 4) | (u32(a.strike) << 5)
        | (u32(a.font) << 6) | (u32(a.dwl) << 7) | (u32(a.dhl) << 8)
        | (u32(a.smallFont) << 9) | (u32(a.baseline) << 10)
        | (u32(a.protectedCell) << 11) | (u32(a.dim) << 12) | (u32(a.conceal) << 13);
}
void restore(CellAttributes& a, u32 b) {
    a.bold = (b & 1) != 0; a.underline = (b & 2) != 0; a.italic = (b & 4) != 0;
    a.blink = (b & 8) != 0; a.reverse = (b & 16) != 0; a.strike = (b & 32) != 0;
    a.font = (b & 64) != 0; a.dwl = (b & 128) != 0; a.dhl = (b & 256) != 0;
    a.smallFont = (b & 512) != 0; a.baseline = (b & 1024) != 0;
    a.protectedCell = (b & 2048) != 0; a.dim = (b & 4096) != 0; a.conceal = (b & 8192) != 0;
}
void writeColor(std::vector<char>& bytes, const TerminalColor& color) {
    if (color.type > ColorType::Rgb) throw std::runtime_error("invalid color type");
    put(bytes, u8(color.type), 1); put(bytes, color.index, 1); put(bytes, color.red, 1);
    put(bytes, color.green, 1); put(bytes, color.blue, 1);
}
TerminalColor readColor(const std::vector<char>& bytes, std::size_t& offset) {
    TerminalColor c;
    c.type = ColorType(get(bytes, offset, 1)); c.index = u8(get(bytes, offset, 1));
    c.red = u8(get(bytes, offset, 1)); c.green = u8(get(bytes, offset, 1)); c.blue = u8(get(bytes, offset, 1));
    if (c.type > ColorType::Rgb) throw std::runtime_error("invalid color type");
    return c;
}
void validate(const EncodedHistoryBlock& b, HistoryCodecLimits limits) {
    if (b.version != 1 || (b.encoding != HistoryEncoding::Raw && b.encoding != HistoryEncoding::Lz4))
        throw std::runtime_error("unsupported history format");
    if (b.index.size() > limits.lines || b.serializedBytes > limits.serializedBytes
        || b.serializedBytes > u64(std::numeric_limits<int>::max())
        || b.payload.size() > b.serializedBytes + b.serializedBytes / 255 + 16
        || b.payload.size() > std::size_t(std::numeric_limits<int>::max())
        || b.payloadBytes != b.payload.size()) throw std::runtime_error("history size limit");
    u64 expected = 0; LineId previous = 0;
    for (const auto& row : b.index) {
        if (!row.id || row.id <= previous || row.offset != expected
            || row.cells > (b.serializedBytes - expected) / CellBytes)
            throw std::runtime_error("invalid history index");
        expected += row.cells * CellBytes; previous = row.id;
    }
    if (expected != b.serializedBytes || b.decodedBytes != ScrollbackBlockCodec::decodedCharge(b.index)
        || b.decodedBytes > limits.decodedBytes) throw std::runtime_error("invalid decoded allocation bound");
    if (b.encoding == HistoryEncoding::Raw && b.payloadBytes != expected)
        throw std::runtime_error("raw history length mismatch");
}
} // namespace
std::size_t EncodedHistoryBlock::byteSize() const {
    return sizeof(*this) + 64 + index.capacity() * sizeof(HistoryLineIndex)
        + (index.capacity() ? 64 : 0) + payload.capacity() + (payload.capacity() ? 64 : 0);
}
std::size_t ScrollbackBlockCodec::decodedCharge(const std::vector<HistoryLineIndex>& index) {
    std::size_t total = sizeof(std::vector<LogicalLine>) + 64;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    for (const auto& row : index) {
        if (row.cells > (maximum - 256 - sizeof(LogicalLine) * 2) / sizeof(Cell))
            throw std::runtime_error("decoded allocation overflow");
        const auto bytes = std::size_t(row.cells) * sizeof(Cell);
        const auto charge = 2 * sizeof(LogicalLine) + (bytes ? ((bytes + 15) / 16) * 16 + 64 : 0);
        if (total > maximum - charge) throw std::runtime_error("decoded allocation overflow");
        total += charge;
    }
    return total;
}
HistoryEncodeResult ScrollbackBlockCodec::encode(const std::vector<LogicalLine>& lines, HistoryCodecLimits limits) {
    HistoryEncodeResult result;
    try {
        auto& b = result.block;
        if (lines.size() > limits.lines) throw std::runtime_error("history line limit");
        b.index.reserve(lines.size());
        for (const auto& line : lines) {
            if (line.cells.size() > (limits.serializedBytes - std::min<u64>(b.serializedBytes, limits.serializedBytes)) / CellBytes)
                throw std::runtime_error("history serialized limit");
            b.index.push_back({line.id, u64(line.cells.size()), b.serializedBytes, line.hardBreak});
            b.serializedBytes += u64(line.cells.size()) * CellBytes;
        }
        b.decodedBytes = decodedCharge(b.index);
        validate(b, limits);
        std::vector<char> bytes; bytes.reserve(std::size_t(b.serializedBytes));
        for (const auto& line : lines) for (const auto& cell : line.cells) {
            for (auto cp : cell.chars) put(bytes, cp, 4);
            if (cell.width > 2 || cell.attributes.underlineStyle > UnderlineStyle::Curly)
                throw std::runtime_error("invalid cell fields");
            put(bytes, cell.width, 1); put(bytes, attributes(cell.attributes), 2);
            put(bytes, u8(cell.attributes.underlineStyle), 1);
            writeColor(bytes, cell.foreground); writeColor(bytes, cell.background);
        }
        if (bytes.empty()) { b.encoding = HistoryEncoding::Raw; return result; }
        const int bound = LZ4_compressBound(int(bytes.size()));
        if (bound <= 0) throw std::runtime_error("history exceeds LZ4 input range");
        std::vector<char> output(std::size_t(bound), '\0');
        const int length = LZ4_compress_default(bytes.data(), output.data(), int(bytes.size()), bound);
        if (length <= 0) throw std::runtime_error("history compression failed");
        output.resize(std::size_t(length)); output.shrink_to_fit();
        b.payload = std::move(output); b.payloadBytes = b.payload.size();
    } catch (const std::exception& e) { result.block = {}; result.error = e.what(); }
    return result;
}
HistoryDecodeResult ScrollbackBlockCodec::decode(const EncodedHistoryBlock& b, HistoryCodecLimits limits) {
    HistoryDecodeResult result;
    try {
        validate(b, limits);
        std::vector<char> bytes(std::size_t(b.serializedBytes), '\0');
        if (b.encoding == HistoryEncoding::Raw) std::copy(b.payload.begin(), b.payload.end(), bytes.begin());
        else if (bytes.empty() || LZ4_decompress_safe(b.payload.data(), bytes.data(), int(b.payload.size()), int(bytes.size())) != int(bytes.size()))
            throw std::runtime_error("history decompression length mismatch");
        result.lines.reserve(b.index.size());
        std::size_t offset = 0;
        for (const auto& row : b.index) {
            LogicalLine line; line.id = row.id; line.hardBreak = row.hardBreak;
            line.cells.resize(std::size_t(row.cells));
            for (auto& cell : line.cells) {
                for (auto& cp : cell.chars) cp = u32(get(bytes, offset, 4));
                cell.width = u8(get(bytes, offset, 1));
                const auto bits = u32(get(bytes, offset, 2)); restore(cell.attributes, bits);
                cell.attributes.underlineStyle = UnderlineStyle(get(bytes, offset, 1));
                if (cell.width > 2 || bits > AttributeMask || cell.attributes.underlineStyle > UnderlineStyle::Curly)
                    throw std::runtime_error("invalid decoded cell fields");
                cell.foreground = readColor(bytes, offset); cell.background = readColor(bytes, offset);
            }
            result.lines.push_back(std::move(line));
        }
    } catch (const std::exception& e) { result.lines.clear(); result.error = e.what(); }
    return result;
}
} // namespace NovaTerm
