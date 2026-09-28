/**
 * @file   TerminalTypes.h
 * @brief  终端核心层共用的基础类型定义。
 *
 * 定义 Cell / CellAttributes / TerminalColor / CursorState 等跨平台数据结构，
 * 被 ScreenBuffer、ScrollbackBuffer、VTAdapter、TerminalCore 与渲染层共用。
 * 不依赖 Qt GUI，便于在单元测试中独立使用。
 */
#pragma once

#include "core/CoreTypes.h"

#include <QMetaType>

#include <array>
#include <cstdint>

namespace NovaTerm {

// 单个 Cell 最多容纳的 Unicode 码点数：基础字符 + 组合标记序列。
inline constexpr int MaxCharsPerCell = 6;
// 宽字符后续 Cell 的占位标记。当 chars[0] 等于此值时，表示当前 Cell
// 是前一个宽字符的视觉延续，不包含独立字形。
inline constexpr uint32_t WideCharContinuation = 0xFFFFFFFFu;

// 屏幕坐标（行、列），原点 (0,0) 位于左上角。
struct Position
{
    int row{0};
    int col{0};
};

inline bool operator==(const Position& lhs, const Position& rhs)
{
    return lhs.row == rhs.row && lhs.col == rhs.col;
}

inline bool operator!=(const Position& lhs, const Position& rhs)
{
    return !(lhs == rhs);
}

// 字典序比较：先按行后按列，便于在 std::map / 排序中使用。
inline bool operator<(const Position& lhs, const Position& rhs)
{
    return lhs.row < rhs.row
        || (lhs.row == rhs.row && lhs.col < rhs.col);
}

// 屏幕脏区域描述。坐标为半开区间 [start, end)，便于表示空区域。
struct DirtyRegion
{
    int startRow{0};
    int endRow{0};
    int startColumn{0};
    int endColumn{0};

    bool isEmpty() const
    {
        return startRow >= endRow || startColumn >= endColumn;
    }
};

// 颜色来源：默认（终端方案）、ANSI 索引色、直接 RGB。
enum class ColorType : uint8_t
{
    Default,
    Indexed,
    Rgb
};

// 终端颜色值。type 决定使用 index（ANSI 16 色）还是 RGB 分量。
struct TerminalColor
{
    ColorType type{ColorType::Default};
    uint8_t index{0};
    uint8_t red{0};
    uint8_t green{0};
    uint8_t blue{0};
};

// 下划线样式，对应 SGR 4 / 21 / 4:2 / 4:3 等 ANSI 扩展序列。
enum class UnderlineStyle : uint8_t
{
    Off,
    Single,
    Double,
    Curly
};

// 单个 Cell 的全部属性位，对应 SGR（Select Graphic Rendition）控制序列。
struct CellAttributes
{
    bool bold{false};
    bool underline{false};
    bool italic{false};
    bool blink{false};
    bool reverse{false};
    bool strike{false};
    bool font{false};          // SGR 11/12：备用字体选择
    bool dwl{false};           // Double Width Line（DEC DWL）
    bool dhl{false};           // Double Height Line（DEC DHL）
    bool smallFont{false};     // SGR 73：小字号
    bool baseline{false};      // SGR 74/75：上/下基线偏移
    // ⚠ protectedCell 与 dim 目前**恒为 false**，没有可靠来源：
    // vendored libvterm 的 VTermScreenCellAttrs（vterm.h:499-510）不提供这两位，
    // 其 SGR 分派（pen.c:291-460）也没有 SGR 2 分支，DECSCA 只用于 erase 回调的
    // selective 参数而不落到 per-cell 状态。要填上它们必须自己跟踪 pen 并知道每个
    // Cell 由哪次 SGR 写入 —— 那要求接管 libvterm 的 screen 层（改用
    // VTermStateCallbacks 的 putglyph），不是适配层能做到的。
    // 字段保留以维持数据契约；在有来源之前不要让渲染或擦除语义依赖它们。
    // 见 docs/architecture/stages/P1_ScreenBuffer_and_VTAdapter.md「剩余工作」。
    bool protectedCell{false}; // DECSCA 保护单元格，清屏时不擦除（暂无来源）
    bool dim{false};           // SGR 2：低亮度（暂无来源）
    bool conceal{false};       // SGR 8：隐藏文本
    UnderlineStyle underlineStyle{UnderlineStyle::Off};
};

// 终端单元格：包含码点序列、显示宽度与全部属性。组合字符存储在 chars[1..]。
struct Cell
{
    std::array<uint32_t, MaxCharsPerCell> chars{};
    uint8_t width{1};
    CellAttributes attributes;
    TerminalColor foreground;
    TerminalColor background;

    // 是否为宽字符的视觉延续 Cell（无独立字形）。
    bool isWideContinuation() const
    {
        return chars[0] == WideCharContinuation;
    }
};

// 光标形状，对应 DECSCUSR（Set Cursor Style）序列。
enum class CursorShape : uint8_t
{
    Block,
    Underline,
    BarLeft
};

// 鼠标跟踪模式，对应 xterm 1000/1002/1003 私有模式（libvterm 的
// VTERM_PROP_MOUSE）。值与 VTermMouseProp 一一对应，但核心层不暴露
// libvterm 类型，故在此自定义。
enum class MouseTrackingMode : uint8_t
{
    None,   // 不上报鼠标事件
    Click,  // ?1000h：按下 / 释放
    Drag,   // ?1002h：按下 / 释放 + 按住拖动
    Move    // ?1003h：任意移动
};

// 光标完整状态：位置、形状、可见性、闪烁。由 VTAdapter 维护并通过信号发布。
struct CursorState
{
    Position position;
    CursorShape shape{CursorShape::Block};
    bool visible{true};
    bool blink{true};
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::DirtyRegion)
