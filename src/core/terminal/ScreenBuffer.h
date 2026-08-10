/**
 * @file   ScreenBuffer.h
 * @brief  终端活动屏幕缓冲区。
 *
 * 表示终端"可视区域"的二维 Cell 矩阵（典型 80x24 / 120x30 等）。
 * 当光标滚出顶行时，被滚出的行进入 ScrollbackBuffer；本类只负责
 * 活动屏幕的存储、resize、矩形拷贝（用于滚动）与清空。
 */
#pragma once

#include "TerminalTypes.h"

#include <QVector>
#include <QSharedPointer>

namespace NovaTerm {

// 终端活动屏幕缓冲。一维存储按行优先展开，便于 swap 与连续访问。
class ScreenBuffer
{
public:
    ScreenBuffer(int columns = 80, int rows = 24);

    /**
     * @brief 调整屏幕尺寸。保留左上角重叠区域内容，多余行/列被丢弃。
     */
    void resize(int columns, int rows);
    int columns() const { return _columns; }
    int rows() const { return _rows; }

    const Cell* cellAt(int row, int column) const;
    Cell* cellAt(int row, int column);
    void setCell(int row, int column, const Cell& cell);

    /**
     * @brief 将 source 矩形内容拷贝到 destination 矩形。
     *        用于光标滚动、区域滚动等场景。源与目标可重叠。
     */
    void moveRect(const DirtyRegion& destination, const DirtyRegion& source);
    void clear();

    const QVector<Cell>& cells() const { return _cells; }

private:
    // 二维坐标 (row, column) 到一维存储索引的转换。越界返回 -1。
    int indexOf(int row, int column) const;

    int _columns{0};
    int _rows{0};
    QVector<Cell> _cells;
};

// 终端快照：包含可见区域全部 Cell 与光标状态。用于测试与一次性渲染。
struct TerminalSnapshot
{
    quint64 revision{0};   // 模型版本号，标识此次发布的不可变性
    int columns{0};
    int rows{0};
    QVector<Cell> visibleCells;
    CursorState cursor;

    const Cell* cellAt(int row, int column) const;
};

// 渲染层专用的稀疏快照。仅请求的 widget 行携带 Cell 数据；活动屏幕
// 与滚动映射在一次模型锁内完成，保证单帧不会混合不同历史版本。
struct RendererSnapshot
{
    quint64 revision{0};
    int columns{0};
    int rows{0};
    QVector<quint64> visibleRowRevisions;
    // 行内容指纹（在模型锁内计算）。渲染器据此判断行是否可复用，
    // 而无需把可变数组下标当作身份标识。
    QVector<quint64> visibleRowIdentities;
    QVector<QSharedPointer<const QVector<Cell>>> visibleRows;
    CursorState cursor;

    const Cell* cellAt(int widgetRow, int column) const;
};

/**
 * @brief 由 ScreenBuffer 与光标状态构造 TerminalSnapshot。
 */
TerminalSnapshot makeSnapshot(const ScreenBuffer& screen,
                              const CursorState& cursor);

} // namespace NovaTerm
