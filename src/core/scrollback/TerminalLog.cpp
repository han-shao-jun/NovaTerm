#include "ChunkedScrollback.h"
#include "ScrollbackChunk.h"
#include <algorithm>

namespace NovaTerm {
namespace {
std::string utf8(uint32_t cp)
{
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    std::string out;
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
        out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    } else {
        out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63));
        out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63));
    }
    return out;
}
}

LogPage ChunkedScrollback::readLog(const LogRequest& request) const
{
    LogPage page;
    page.generation = _logGeneration;
    const auto last = _lineCount ? lineInfoAt(_lineCount - 1) : HistoryLineIndex{};
    // The unfinished logical tail can still grow. It is observed on screen until committed.
    page.sampledEnd.line = last.id && !last.hardBreak ? last.id : _nextLineId;
    page.earliest.line = _lineCount ? lineInfoAt(0).id : page.sampledEnd.line;
    page.next = request.cursor ? request.cursor->position
        : request.start == LogStart::Tail ? page.sampledEnd : page.earliest;
    const auto finish = [&page]() {
        page.hasMore = page.next != page.sampledEnd;
        page.caughtUp = page.status == LogStatus::Ok && !page.hasMore;
        return page;
    };
    if (request.cursor && request.cursor->generation != page.generation) {
        page.status = LogStatus::Invalidated; return finish();
    }
    if (page.next.line < page.earliest.line) { page.status = LogStatus::Gap; return finish(); }
    if (page.next.line > page.sampledEnd.line || (page.next.line == page.sampledEnd.line
        && (page.next.cell || page.next.scalar || page.next.byte))) {
        page.status = LogStatus::InvalidPosition; return finish();
    }
    if (!request.maxBytes || !request.maxLines || request.maxBytes > 262144 || request.maxLines > 1024) {
        page.status = LogStatus::BudgetTooSmall; return finish();
    }
    std::size_t bytes = 0, work = 0;
    const auto locateLine = [this](LineId id) -> std::pair<const StoredChunk*, isize> {
        const auto chunk = std::lower_bound(_chunks.cbegin(), _chunks.cend(), id,
            [](const StoredChunk& value, LineId sought) {
                return chunkLineInfo(*value.chunk, value.firstLine + value.lineCount - 1).id < sought;
            });
        if (chunk != _chunks.cend()) {
            isize lo = chunk->firstLine, hi = lo + chunk->lineCount;
            while (lo < hi) {
                const auto mid = lo + (hi - lo) / 2;
                if (chunkLineInfo(*chunk->chunk, mid).id < id) lo = mid + 1;
                else hi = mid;
            }
            return {&*chunk, lo};
        }
        if (_active) {
            const auto found = std::lower_bound(_active->lines.cbegin() + _activeFirstLine,
                _active->lines.cend(), id,
                [](const LogicalLine& value, LineId sought) { return value.id < sought; });
            if (found != _active->lines.cend()) return {nullptr, isize(found - _active->lines.cbegin())};
        }
        return {nullptr, -1};
    };
    const auto successor = [this, &locateLine, &page](LineId id) {
        const auto found = locateLine(id + 1);
        if (found.second < 0) return page.sampledEnd;
        const auto next = found.first ? chunkLineInfo(*found.first->chunk, found.second).id
            : _active->lines[std::size_t(found.second)].id;
        return LogPosition{next};
    };
    while (page.next.line < page.sampledEnd.line && page.fragments.size() < request.maxLines) {
        // Lookup by stable ID, logarithmic in chunks and rows; never copy the full index.
        const auto found = locateLine(page.next.line);
        PinnedHistoryLine pin;
        const LogicalLine* line = nullptr;
        if (found.first) {
            pin = pinChunkLine(found.first->chunk, found.second, HistoryReadMode::CacheOnly);
            if (!pin) {
                page.status = pin.status() == HistoryReadStatus::Pending ? LogStatus::Pending
                    : pin.status() == HistoryReadStatus::Busy ? LogStatus::Busy : LogStatus::Unavailable;
                return finish();
            }
            line = &*pin;
        } else if (found.second >= 0) line = &_active->lines[std::size_t(found.second)];
        if (!line || line->id != page.next.line) {
            page.status = LogStatus::Unavailable; return finish();
        }
        if (page.next.cell > line->cells.size() || page.next.scalar >= MaxCharsPerCell) {
            page.status = LogStatus::InvalidPosition; return finish();
        }
        // A connection reset may abandon a soft row before later completed rows.
        // It is now immutable, but must not gain an invented newline.
        LogFragment fragment{line->id, page.next.byte, {}, false, line->hardBreak};
        while (page.next.cell < line->cells.size()) {
            if (++work > request.maxBytes * 2 + 1024) break;
            const auto& cell = line->cells[std::size_t(page.next.cell)];
            if (cell.isWideContinuation()) { ++page.next.cell; page.next.scalar = 0; continue; }
            const auto cp = cell.chars[page.next.scalar];
            const auto encoded = utf8(cp ? cp : uint32_t(' '));
            if (bytes + encoded.size() > request.maxBytes) break;
            fragment.text += encoded;
            bytes += encoded.size(); page.next.byte += encoded.size();
            ++page.next.scalar;
            if (page.next.scalar == MaxCharsPerCell || cell.chars[page.next.scalar] == 0) {
                ++page.next.cell; page.next.scalar = 0;
            }
        }
        if (page.next.cell == line->cells.size() && (!line->hardBreak || bytes < request.maxBytes)) {
            if (line->hardBreak) { fragment.text += '\n'; ++bytes; }
            fragment.lineComplete = true;
            page.next = successor(line->id);
        }
        const bool complete = fragment.lineComplete;
        if (!fragment.text.empty() || complete) page.fragments.push_back(std::move(fragment));
        else {
            if (page.fragments.empty()) page.status = LogStatus::BudgetTooSmall;
            break;
        }
        if (page.status != LogStatus::Ok || !complete
            || bytes == request.maxBytes || work > request.maxBytes * 2 + 1024) break;
    }
    return finish();
}
} // namespace NovaTerm
