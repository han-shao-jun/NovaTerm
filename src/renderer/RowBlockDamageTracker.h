/**
 * @file RowBlockDamageTracker.h
 * @brief Reconciles scheduled terminal damage with cached renderer blocks.
 */
#pragma once

#include "RenderCommandBuffer.h"
#include "core/terminal/TerminalTypes.h"

#include <QVector>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace NovaTerm {

// Tracks the renderer's actual per-block contents. Terminal damage rectangles
// are scheduling hints; comparing against the final snapshot prevents a short
// cursor-positioned rewrite from leaving commands from an older, longer row.
class RowBlockDamageTracker
{
public:
    static constexpr int BlockColumns = 8;

    void reset(int rows, int columns)
    {
        _columns = std::max(0, columns);
        const int blockCount = (_columns + BlockColumns - 1) / BlockColumns;
        _rowHashes.resize(std::max(0, rows));
        _validRows.fill(false, _rowHashes.size());
        for (auto& hashes : _rowHashes)
            hashes.fill(0, blockCount);
    }

    void rotateRowsUp(int count)
    {
        if (_rowHashes.isEmpty())
            return;
        count = std::clamp(count, 0, int(_rowHashes.size()));
        if (count == 0)
            return;
        std::rotate(_rowHashes.begin(), _rowHashes.begin() + count,
                    _rowHashes.end());
        std::rotate(_validRows.begin(), _validRows.begin() + count,
                    _validRows.end());
        for (int row = int(_rowHashes.size()) - count;
             row < _rowHashes.size(); ++row) {
            _validRows[row] = false;
        }
    }

    QVector<DirtyColumnSpan> reconcileRow(
        int row, const Cell* cells, int columns,
        QVector<DirtyColumnSpan> requestedSpans)
    {
        columns = std::max(0, columns);
        if (row < 0 || row >= _rowHashes.size() || columns != _columns
            || !cells) {
            return columns > 0
                ? QVector<DirtyColumnSpan>{{0, columns}}
                : QVector<DirtyColumnSpan>{};
        }

        auto& cached = _rowHashes[row];
        const int blockCount = (columns + BlockColumns - 1) / BlockColumns;
        if (cached.size() != blockCount) {
            cached.fill(0, blockCount);
            _validRows[row] = false;
        }

        for (int block = 0; block < blockCount; ++block) {
            const int start = block * BlockColumns;
            const int end = std::min(columns, start + BlockColumns);
            const quint64 current = blockIdentity(cells + start, end - start);
            if (!_validRows[row] || cached[block] != current)
                requestedSpans.push_back({start, end});
            cached[block] = current;
        }
        _validRows[row] = true;
        return mergeSpans(std::move(requestedSpans), columns);
    }

private:
    static void mix(quint64& hash, quint64 value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }

    static quint64 blockIdentity(const Cell* cells, int count)
    {
        quint64 hash = 1469598103934665603ull;
        for (int index = 0; index < count; ++index) {
            const Cell& cell = cells[index];
            for (const uint32_t scalar : cell.chars)
                mix(hash, scalar);
            mix(hash, cell.width);
            mix(hash, quint8(cell.foreground.type));
            mix(hash, cell.foreground.index);
            mix(hash, cell.foreground.red | (cell.foreground.green << 8)
                          | (cell.foreground.blue << 16));
            mix(hash, quint8(cell.background.type));
            mix(hash, cell.background.index);
            mix(hash, cell.background.red | (cell.background.green << 8)
                          | (cell.background.blue << 16));
            const auto& attributes = cell.attributes;
            const quint64 flags = quint64(attributes.bold)
                | (quint64(attributes.underline) << 1)
                | (quint64(attributes.italic) << 2)
                | (quint64(attributes.blink) << 3)
                | (quint64(attributes.reverse) << 4)
                | (quint64(attributes.strike) << 5)
                | (quint64(attributes.font) << 6)
                | (quint64(attributes.dwl) << 7)
                | (quint64(attributes.dhl) << 8)
                | (quint64(attributes.smallFont) << 9)
                | (quint64(attributes.baseline) << 10)
                | (quint64(attributes.protectedCell) << 11)
                | (quint64(attributes.dim) << 12)
                | (quint64(attributes.conceal) << 13)
                | (quint64(attributes.underlineStyle) << 14);
            mix(hash, flags);
        }
        return hash;
    }

    static QVector<DirtyColumnSpan> mergeSpans(
        QVector<DirtyColumnSpan> spans, int columns)
    {
        for (auto& span : spans) {
            span.startColumn = std::clamp(span.startColumn, 0, columns);
            span.endColumn = std::clamp(span.endColumn,
                                        span.startColumn, columns);
        }
        spans.erase(std::remove_if(spans.begin(), spans.end(),
                                   [](const DirtyColumnSpan& span) {
                                       return span.startColumn >= span.endColumn;
                                   }),
                    spans.end());
        std::sort(spans.begin(), spans.end(),
                  [](const DirtyColumnSpan& left,
                     const DirtyColumnSpan& right) {
                      return left.startColumn < right.startColumn;
                  });
        QVector<DirtyColumnSpan> merged;
        for (const auto& span : spans) {
            if (merged.isEmpty()
                || span.startColumn > merged.back().endColumn) {
                merged.push_back(span);
            } else {
                merged.back().endColumn = std::max(
                    merged.back().endColumn, span.endColumn);
            }
        }
        return merged;
    }

    int _columns{0};
    QVector<QVector<quint64>> _rowHashes;
    QVector<quint8> _validRows;
};

} // namespace NovaTerm
