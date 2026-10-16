#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NovaTerm {
enum class InputKey : std::uint8_t {
    None, Enter, Tab, Backspace, Escape, Up, Down, Left, Right,
    Insert, Delete, Home, End, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12
};
struct TerminalInputAction {
    enum class Kind { Text, Key, Paste, Bytes };
    Kind kind{Kind::Text};
    std::string text;
    InputKey key{InputKey::None};
    unsigned modifiers{0}; // Shift=1, Alt=2, Ctrl=4, independent of Qt/libvterm.
};
}
