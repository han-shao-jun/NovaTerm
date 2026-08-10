/**
 * @file   TerminalTypes.h
 * @brief  终端核心层共用的基础类型定义。
 *
 * 定义 Cell / CellAttributes / TerminalColor / CursorState 等跨平台数据结构，
 * 被 ScreenBuffer、ScrollbackBuffer、VTAdapter、TerminalCore 与渲染层共用。
 * 不依赖 Qt GUI，便于在单元测试中独立使用。
 */
#pragma once

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
    bool dwl{false};           // 双倍宽度行（DEC DWL）
    bool dhl{false};           // 双倍高度行（DEC DHL）
    bool smallFont{false};     // SGR 73：小字号
    bool baseline{false};      // SGR 74/75：上/下基线偏移
    bool protectedCell{false}; // DECSCA 保护单元格，清屏时不擦除
    bool dim{false};           // SGR 2：低亮度
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
