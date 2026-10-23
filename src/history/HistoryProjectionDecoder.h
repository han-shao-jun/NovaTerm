#pragma once
#include "HistoryQueryTypes.h"
#include <deque>
namespace NovaTerm::Recording {
// Stateful UTF-8 line projection. ANSI is stripped; CR overwrites character cells.
// The original byte locations survive chunk/record splits. No terminal state is sampled.
class HistoryProjectionDecoder final {
public:
    struct Unit { QByteArray bytes; std::vector<RawSpan> spans; };
    bool feed(char byte, RawSpan origin, const std::function<void(Unit)>& output);
    void flush(const std::function<void(Unit)>& output);
    void reset();
    const std::vector<RawSpan>& limitRange() const { return _limit; }
private:
    struct Glyph { uint codepoint; std::vector<RawSpan> spans; };
    enum class Escape { None, Esc, Csi, Osc, OscEsc };
    std::vector<Glyph> _line;
    std::vector<RawSpan> _pendingOrigins;
    QByteArray _pending;
    std::vector<RawSpan> _limit;
    size_t _column{0};
    int _sequenceBytes{0}, _rawLineBytes{0};
    Escape _escape{Escape::None};
    void glyph(uint, std::vector<RawSpan>);
    void pending(bool final);
    void line(const std::function<void(Unit)>&);
};
class HistoryLiteralMatcher final {
public:
    explicit HistoryLiteralMatcher(QByteArray query = {});
    std::optional<HistoryMatch> feed(char, RawSpan);
    void reset();
private:
    QByteArray _query;
    std::vector<int> _prefix;
    std::deque<RawSpan> _origins;
    int _matched{0};
};
void appendSpan(std::vector<RawSpan>&, RawSpan);
}
