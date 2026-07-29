#pragma once

#include <QMetaType>

#include <array>
#include <cstdint>

namespace NovaTerm {

inline constexpr int MaxCharsPerCell = 6;
inline constexpr uint32_t WideCharContinuation = 0xFFFFFFFFu;

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

inline bool operator<(const Position& lhs, const Position& rhs)
{
    return lhs.row < rhs.row
        || (lhs.row == rhs.row && lhs.col < rhs.col);
}

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

enum class ColorType : uint8_t
{
    Default,
    Indexed,
    Rgb
};

struct TerminalColor
{
    ColorType type{ColorType::Default};
    uint8_t index{0};
    uint8_t red{0};
    uint8_t green{0};
    uint8_t blue{0};
};

enum class UnderlineStyle : uint8_t
{
    Off,
    Single,
    Double,
    Curly
};

struct CellAttributes
{
    bool bold{false};
    bool underline{false};
    bool italic{false};
    bool blink{false};
    bool reverse{false};
    bool strike{false};
    bool font{false};
    bool dwl{false};
    bool dhl{false};
    bool smallFont{false};
    bool baseline{false};
    bool protectedCell{false};
    bool dim{false};
    bool conceal{false};
    UnderlineStyle underlineStyle{UnderlineStyle::Off};
};

struct Cell
{
    std::array<uint32_t, MaxCharsPerCell> chars{};
    uint8_t width{1};
    CellAttributes attributes;
    TerminalColor foreground;
    TerminalColor background;

    bool isWideContinuation() const
    {
        return chars[0] == WideCharContinuation;
    }
};

enum class CursorShape : uint8_t
{
    Block,
    Underline,
    BarLeft
};

struct CursorState
{
    Position position;
    CursorShape shape{CursorShape::Block};
    bool visible{true};
    bool blink{true};
};

} // namespace NovaTerm

Q_DECLARE_METATYPE(NovaTerm::DirtyRegion)
