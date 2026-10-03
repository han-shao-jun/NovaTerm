/** @file ZmodemCodec.h @brief 有界 ZMODEM 头与数据子包增量编解码。 */
#pragma once
#include "TransferTypes.h"
#include <array>
namespace NovaTerm::FileTransfer {
enum class ZFrame : std::uint8_t {
    RequestInit=0, ReceiveInit=1, SendInit=2, Ack=3, File=4, Skip=5, Nak=6,
    Abort=7, Finish=8, ResumePosition=9, Data=10, Eof=11, FileError=12,
    Crc=13, Challenge=14, Complete=15, Cancel=16, FreeCount=17, Command=18
};
enum class ZHeaderFormat { Hex, Binary16, Binary32 };
enum class ZEnd : std::uint8_t { End='h', Continue='i', Ack='j', Wait='k' };
enum class ZEventKind { Header, Data, Error, Cancel };
struct ZEvent {
    ZEventKind kind{ZEventKind::Error};
    ZFrame frame{ZFrame::RequestInit};
    std::uint32_t position{0};
    bool crc32{false};
    ZEnd end{ZEnd::End};
    Bytes bytes;
};
/** @brief 逐字节解析器；输入不缓存，数据与 CRC 暂存上限 8196 字节。 */
class ZmodemCodec {
public:
    [[nodiscard]] static Bytes header(ZFrame frame, std::uint32_t position,
                                      ZHeaderFormat format);
    [[nodiscard]] static Bytes data(ByteView payload, ZEnd end, bool crc32);
    [[nodiscard]] std::optional<ZEvent> feed(std::uint8_t byte);
    void reset();
    /** @brief 显式进入数据子包态；结束标记后等待引擎决定下一种帧。 */
    void expectData(bool crc32, std::size_t maxBytes);
private:
    enum class Mode { Search, Pad, Indicator, Header, HexCr, HexLf, Data, DataCrc };
    [[nodiscard]] std::optional<ZEvent> decoded(std::uint8_t byte);
    [[nodiscard]] std::optional<ZEvent> error();
    [[nodiscard]] std::optional<ZEvent> finishHeader();
    [[nodiscard]] std::optional<ZEvent> finishData();
    Mode _mode{Mode::Search};
    ZHeaderFormat _format{ZHeaderFormat::Hex};
    Bytes _bytes;
    std::size_t _maxData{8192};
    std::size_t _crcBytes{0};
    bool _escape{false};
    int _hexNibble{-1};
    unsigned _cancelCount{0};
    ZEnd _end{ZEnd::End};
};
} // namespace NovaTerm::FileTransfer
