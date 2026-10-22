#pragma once
#include "HistorySearchTypes.h"
#include <deque>
namespace NovaTerm::Recording {
struct HistoryProjectedUnit { char32_t value{0}; std::vector<HistoryRawRange> ranges; };
// Stateful UTF-8 / ANSI stripping / CR overwrite projection, bounded to one line.
class HistoryProjectionDecoder final {
public:
    void feed(unsigned char byte, quint64 recordId, qsizetype byteOffset);
    void finishLine();
    void reset();
    std::deque<HistoryProjectedUnit> takeReady();
    bool limited() const { return _limited; }
    size_t memoryBytes() const;
private:
    enum class Escape { None, Esc, Csi, Osc, OscEsc };
    Escape _escape{Escape::None};
    size_t _controlBytes{0}, _lineBytes{0}, _column{0};
    std::deque<HistoryProjectedUnit> _line;
    std::deque<HistoryProjectedUnit> _ready;
    size_t _lineRangeBytes{0}, _readyRangeBytes{0};
    QByteArray _utf8;
    std::vector<HistoryRawRange> _utf8Ranges;
    int _expected{0};
    bool _limited{false};
    void emitUnit(char32_t, std::vector<HistoryRawRange>);
    void flushUtf8();
};
}
