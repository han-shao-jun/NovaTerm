#include "HistoryProjectionDecoder.h"
#include <algorithm>
#include <utility>
namespace NovaTerm::Recording {
namespace {
void appendRange(std::vector<HistoryRawRange>& ranges, HistoryRawRange next) {
    if (!ranges.empty() && ranges.back().recordId == next.recordId
        && ranges.back().byteOffset + ranges.back().byteCount == next.byteOffset) ranges.back().byteCount += next.byteCount;
    else ranges.push_back(next);
}
}
void HistoryProjectionDecoder::emitUnit(char32_t value, std::vector<HistoryRawRange> ranges) {
    const auto rangeBytes = ranges.capacity() * sizeof(HistoryRawRange);
    HistoryProjectedUnit unit{value, std::move(ranges)};
    if (_column < _line.size()) {
        _lineRangeBytes -= _line[_column].ranges.capacity() * sizeof(HistoryRawRange);
        _line[_column] = std::move(unit);
    } else _line.push_back(std::move(unit));
    _lineRangeBytes += rangeBytes;
    ++_column;
    // Leave space for matcher tail, an input block and the bounded previous page.
    if (memoryBytes() > 384 * 1024) _limited = true;
}
void HistoryProjectionDecoder::flushUtf8() {
    if (!_utf8.isEmpty()) emitUnit(U'\ufffd', std::move(_utf8Ranges));
    _utf8.clear(); _utf8Ranges.clear(); _expected = 0;
}
void HistoryProjectionDecoder::feed(unsigned char byte, quint64 recordId, qsizetype offset) {
    if (_limited) return;
    if (++_lineBytes > 64 * 1024) { _limited = true; return; }
    if (_escape != Escape::None) {
        if (++_controlBytes > 4096) { _limited = true; return; }
        if (byte == 0x18 || byte == 0x1a) { _escape = Escape::None; return; }
        switch (_escape) {
        case Escape::Esc:
            if (byte == '[') _escape = Escape::Csi;
            else if (byte == ']') _escape = Escape::Osc;
            else if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_') _escape = Escape::String;
            else if (byte >= 0x20 && byte <= 0x2f) _escape = Escape::Intermediate;
            else if (byte == 0x1b) _controlBytes = 1;
            else _escape = Escape::None;
            break;
        case Escape::Intermediate:
            if (byte == 0x1b) { _escape = Escape::Esc; _controlBytes = 1; }
            else if (byte >= 0x30 && byte <= 0x7e) _escape = Escape::None;
            break;
        case Escape::Csi:
            if (byte == 0x1b) { _escape = Escape::Esc; _controlBytes = 1; }
            else if (byte >= 0x40 && byte <= 0x7e) _escape = Escape::None;
            break;
        case Escape::Osc:
            if (byte == 7) _escape = Escape::None;
            else if (byte == 0x1b) _escape = Escape::OscEsc;
            break;
        case Escape::OscEsc: _escape = byte == '\\' ? Escape::None : Escape::Osc; break;
        case Escape::String: if (byte == 0x1b) _escape = Escape::StringEsc; break;
        case Escape::StringEsc:
            if (byte == '\\') _escape = Escape::None;
            else if (byte != 0x1b) _escape = Escape::String;
            break;
        case Escape::None: break;
        }
        return;
    }
    if (!_utf8.isEmpty()) {
        if (byte >= 0x80 && byte <= 0xbf) {
            _utf8.append(char(byte)); appendRange(_utf8Ranges, {recordId, offset, 1});
            if (_utf8.size() == _expected) {
                const auto first = static_cast<unsigned char>(_utf8[0]);
                char32_t cp = first & (_expected == 2 ? 0x1f : _expected == 3 ? 0x0f : 0x07);
                for (int i = 1; i < _expected; ++i) cp = (cp << 6) | (static_cast<unsigned char>(_utf8[i]) & 0x3f);
                if ((_expected == 2 && cp < 0x80) || (_expected == 3 && cp < 0x800)
                    || (_expected == 4 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = U'\ufffd';
                emitUnit(cp, std::move(_utf8Ranges)); _utf8.clear(); _utf8Ranges.clear(); _expected = 0;
            }
            return;
        }
        flushUtf8();
    }
    if (byte == 0x1b) { _escape = Escape::Esc; _controlBytes = 1; return; }
    if (byte == '\r') { _column = 0; return; }
    if (byte == '\n') {
        // LF does not overwrite the current CR column: it commits the whole visible line.
        _column = _line.size(); emitUnit(U'\n', {{recordId, offset, 1}}); finishLine(); return;
    }
    if (byte == '\b') { if (_column) --_column; return; }
    if (byte < 0x20 && byte != '\t') return;
    if (byte < 0x80) { emitUnit(byte, {{recordId, offset, 1}}); return; }
    if (byte >= 0xc2 && byte <= 0xf4) {
        _expected = byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
        _utf8 = QByteArray(1, char(byte)); _utf8Ranges = {{recordId, offset, 1}}; return;
    }
    emitUnit(U'\ufffd', {{recordId, offset, 1}});
}
void HistoryProjectionDecoder::finishLine() {
    flushUtf8();
    if (_ready.empty()) {
        _ready.swap(_line); _readyRangeBytes = std::exchange(_lineRangeBytes, size_t(0));
    } else {
        for (auto& unit : _line) _ready.push_back(std::move(unit));
        _line.clear(); _readyRangeBytes += std::exchange(_lineRangeBytes, size_t(0));
    }
    _column = 0; _lineBytes = 0;
}
void HistoryProjectionDecoder::reset() {
    _escape = Escape::None; _controlBytes = _lineBytes = _column = 0;
    _line.clear(); _ready.clear(); _utf8.clear(); _utf8Ranges.clear(); _expected = 0; _limited = false;
    _lineRangeBytes = _readyRangeBytes = 0;
}
std::deque<HistoryProjectedUnit> HistoryProjectionDecoder::takeReady() {
    std::deque<HistoryProjectedUnit> result; result.swap(_ready); _readyRangeBytes = 0; return result;
}
size_t HistoryProjectionDecoder::memoryBytes() const {
    size_t result = (_line.size() + _ready.size()) * sizeof(HistoryProjectedUnit) + size_t(_utf8.capacity());
    return result + _lineRangeBytes + _readyRangeBytes + _utf8Ranges.capacity() * sizeof(HistoryRawRange);
}
}
