/**
 * @file HistoryLayout.h
 * @brief 显示行逻辑索引；头部淘汰不逐次移动保留历史。
 */
#pragma once
#include "core/scrollback/LineLayout.h"
#include <QVector>
#include <utility>

class HistoryLayout final
{
public:
    [[nodiscard]] qsizetype size() const { return _rows.size() - _head; }
    [[nodiscard]] bool isEmpty() const { return size() == 0; }
    [[nodiscard]] const NovaTerm::DisplayLine& operator[](qsizetype index) const
    { return _rows[_head + index]; }
    [[nodiscard]] const NovaTerm::DisplayLine& constLast() const
    { return _rows.constLast(); }
    void clear() { _rows.clear(); _head = 0; }
    void push_back(const NovaTerm::DisplayLine& row) { _rows.push_back(row); }
    HistoryLayout& operator=(QVector<NovaTerm::DisplayLine>&& rows)
    { _rows = std::move(rows); _head = 0; return *this; }
    void remove(qsizetype first, qsizetype count)
    {
        Q_ASSERT(first >= 0 && count >= 0 && first + count <= size());
        if (first != 0) {
            _rows.remove(_head + first, count);
            return;
        }
        _head += count;
        if (_head == _rows.size()) {
            clear();
        } else if (_head >= CompactThreshold && _head >= _rows.size() / 2) {
            _rows.remove(0, _head);
            _head = 0;
        }
    }
private:
    static constexpr qsizetype CompactThreshold = 4096;
    QVector<NovaTerm::DisplayLine> _rows;
    qsizetype _head{0};
};
