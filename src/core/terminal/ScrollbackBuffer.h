#pragma once
#include "core/scrollback/ChunkedScrollback.h"
#include <QVector>

using ScrollbackCell = NovaTerm::Cell;

// Compatibility facade used by libvterm and existing renderer callers. The
// only backing store is ChunkedScrollback; this class does not dual-write the
// retired line-level ring buffer.
class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(int maxLines = 1000);

    void pushLine(const NovaTerm::Cell* cells, int cols);
    QVector<NovaTerm::Cell>& beginPushLine(int columns, int storedColumns);
    void commitPushLine(bool continuation = false, bool hardBreak = true);
    bool popLine(NovaTerm::Cell* cells, int cols);

    // ── 查询 ──
    int lineCount() const;                           // 当前存储行数
    int columns() const { return _cols; }
    const ScrollbackCell* lineAt(int index) const;   // index=0 是最旧的行
    const QVector<ScrollbackCell>* lineVectorAt(int index) const;
    int maxLines() const { return _maxLines; }
    NovaTerm::ScrollbackSnapshot snapshot();
    NovaTerm::ScrollbackStatistics statistics() const;

    // ── 修改 ──
    void setMaxLines(int max);
    void clear();

private:
    NovaTerm::ChunkedScrollback _storage;
    QVector<ScrollbackCell> _pendingLine;
    int _maxLines;
    int _cols{0};
};
