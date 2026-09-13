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

#include <memory>
#include <vector>

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
     * @brief 取一行内连续一段 Cell 的可写首地址，用于按行批量同步。
     * @param row 行号。
     * @param startColumn 起始列（含）。
     * @param count 需要写入的 Cell 数。
     * @return 区间首地址；行号或 [startColumn, startColumn + count) 越界时
     *         返回 nullptr，调用方应放弃本次批量写入。
     * @note 只做一次边界检查与一次行基址计算，替代逐格 setCell() 的
     *       每格 indexOf；调用方保证不越界写入 count 个 Cell。
     */
    Cell* writableRowSpan(int row, int startColumn, int count);

    /**
     * @brief 该行是否为上一行的软换行延续。
     * @note  由 VTAdapter 从 libvterm 的 VTermLineInfo::continuation 同步。
     *        复制与搜索需要它区分行边界是软换行还是硬换行。
     */
    [[nodiscard]] bool rowContinuation(int row) const;
    void setRowContinuation(int row, bool continuation);

    /**
     * @brief 将 source 矩形内容拷贝到 destination 矩形。
     *        用于光标滚动、区域滚动等场景。源与目标可重叠。
     */
    void moveRect(const DirtyRegion& destination, const DirtyRegion& source);
    void clear();

    const std::vector<Cell>& cells() const { return _cells; }

private:
    // 二维坐标 (row, column) 到一维存储索引的转换。越界返回 -1。
    int indexOf(int row, int column) const;

    int _columns{0};
    int _rows{0};
    std::vector<Cell> _cells;
    // 每行一位软换行标志，长度恒等于 _rows。用 u8 而非 bool，避免
    // vector<bool> 位压缩带来的读写开销。
    std::vector<u8> _rowContinuation;
};

// 终端快照：包含可见区域全部 Cell 与光标状态。用于测试与一次性渲染。
struct TerminalSnapshot
{
    u64 revision{0};   // 模型版本号，标识此次发布的不可变性
    int columns{0};
    int rows{0};
    std::vector<Cell> visibleCells;
    CursorState cursor;

    const Cell* cellAt(int row, int column) const;
};

// 渲染层专用的稀疏快照。仅请求的 widget 行携带 Cell 数据；活动屏幕
// 与滚动映射在一次模型锁内完成，保证单帧不会混合不同历史版本。
struct RendererSnapshot
{
    static constexpr int IdentityBlockColumns = 8;

    u64 revision{0};
    int columns{0};
    int rows{0};
    std::vector<u64> visibleRowRevisions;
    // 行内容指纹（在模型锁内计算）。渲染器据此判断行是否可复用，
    // 而无需把可变数组下标当作身份标识。
    std::vector<u64> visibleRowIdentities;
    // 仅携带Cell数据的行同时提供逐8列内容指纹，供Renderer校正遗漏脏区；
    // 与整行identity在Core内由同一次Cell字段遍历生成。
    std::vector<std::vector<u64>> visibleRowBlockIdentities;
    std::vector<std::shared_ptr<const std::vector<Cell>>> visibleRows;
    CursorState cursor;

    const Cell* cellAt(int widgetRow, int column) const;
};

/**
 * @brief 由 ScreenBuffer 与光标状态构造 TerminalSnapshot。
 */
TerminalSnapshot makeSnapshot(const ScreenBuffer& screen,
                              const CursorState& cursor);

} // namespace NovaTerm
