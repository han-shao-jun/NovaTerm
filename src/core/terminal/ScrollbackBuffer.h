#pragma once
#include "TerminalTypes.h"
#include <QVector>

using ScrollbackCell = NovaTerm::Cell;

// 环形缓冲区管理的终端回滚历史行。
// sb_pushline → pushLine() 存入，sb_popline → popLine() 取出恢复。
class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(int maxLines = 1000);

    void pushLine(const NovaTerm::Cell* cells, int cols);
    QVector<NovaTerm::Cell>& beginPushLine(int columns, int storedColumns);
    void commitPushLine();
    bool popLine(NovaTerm::Cell* cells, int cols);

    // ── 查询 ──
    int lineCount() const;                           // 当前存储行数
    int columns() const { return _cols; }
    const ScrollbackCell* lineAt(int index) const;   // index=0 是最旧的行
    const QVector<ScrollbackCell>* lineVectorAt(int index) const;
    int maxLines() const { return _maxLines; }

    // ── 修改 ──
    void setMaxLines(int max);
    void clear();

private:
    QVector<QVector<ScrollbackCell>> _lines;  // 固定大小环形缓冲
    int _maxLines;
    int _cols{0};
    int _writePos{0};   // 下一个写入位置
    int _count{0};      // 当前有效行数
};
