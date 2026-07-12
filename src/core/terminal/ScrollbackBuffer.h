#pragma once
#include <vterm.h>
#include <QVector>

// libvterm 的 sb_pushline 回调传入的 VTermScreenCell 副本。
// 每个 cell 存储完整的字符 + 属性 + 颜色信息，供渲染器回看历史。
struct ScrollbackCell {
    uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
    char width;
    VTermScreenCellAttrs attrs;
    VTermColor fg;
    VTermColor bg;
};

// 环形缓冲区管理的终端回滚历史行。
// sb_pushline → pushLine() 存入，sb_popline → popLine() 取出恢复。
class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(int maxLines = 1000);

    // ── libvterm 回调 ──
    void pushLine(const VTermScreenCell* cells, int cols);
    bool popLine(VTermScreenCell* cells, int cols);

    // ── 查询 ──
    int lineCount() const;                           // 当前存储行数
    int columns() const { return _cols; }
    const ScrollbackCell* lineAt(int index) const;   // index=0 是最旧的行
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
