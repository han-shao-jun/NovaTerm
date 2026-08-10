/**
 * @file   ScrollbackBuffer.h
 * @brief  滚动历史缓冲外观层。
 *
 * libvterm 与现有渲染调用方使用本外观访问滚动历史。实际唯一后端是
 * ChunkedScrollback；本类不再额外维护已弃用的逐行环形缓冲。
 */
#pragma once
#include "core/scrollback/ChunkedScrollback.h"
#include <QVector>

using ScrollbackCell = NovaTerm::Cell;

// libvterm 与现有渲染调用方使用的滚动历史外观。
// 唯一后端为 ChunkedScrollback；本类不双写已弃用的逐行环形缓冲。
class ScrollbackBuffer {
public:
    explicit ScrollbackBuffer(int maxLines = 1000);

    /**
     * @brief 推入一行 Cell（硬换行）。
     * @param cells 行内 Cell 数组，可以为 null（此时忽略）。
     * @param cols Cell 数量。
     */
    void pushLine(const NovaTerm::Cell* cells, int cols);

    /**
     * @brief 开始一次"分批推入"：返回可填充的行缓冲引用。
     *        调用方填完 cells 后调用 commitPushLine() 提交。
     * @param columns 逻辑列数。
     * @param storedColumns 实际存储列数（可能小于 columns）。
     */
    QVector<NovaTerm::Cell>& beginPushLine(int columns, int storedColumns);

    /**
     * @brief 提交 beginPushLine 开始的行。
     * @param continuation 是否为前一行的软换行延续。
     * @param hardBreak 是否为硬换行（行尾有 \n）。
     */
    void commitPushLine(bool continuation = false, bool hardBreak = true);

    /**
     * @brief 弹出最旧的一行（用于上限淘汰测试）。
     * @return true 表示成功弹出；false 表示缓冲为空。
     */
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
    QVector<ScrollbackCell> _pendingLine;  // beginPushLine 的临时缓冲
    int _maxLines;
    int _cols{0};
};
