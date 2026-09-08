/**
 * @file   KeyMapper.cpp
 * @brief  核心输入类型 → libvterm 键码/修饰符映射实现。
 *
 * 详见 KeyMapper.h 的接口说明。映射表为静态 switch，无运行时状态，
 * 不依赖 Qt。
 */
#include "KeyMapper.h"

namespace KeyMapper {

bool keyToVTermKey(NovaTerm::Key key, VTermKey& outKey)
{
    using NovaTerm::Key;
    switch (key) {
    // 编辑键：Enter / Tab / Backspace / Escape
    case Key::Enter:     outKey = VTERM_KEY_ENTER;     return true;
    case Key::Tab:       outKey = VTERM_KEY_TAB;       return true;
    case Key::Backspace: outKey = VTERM_KEY_BACKSPACE; return true;
    case Key::Escape:    outKey = VTERM_KEY_ESCAPE;    return true;

    // 方向键
    case Key::Up:        outKey = VTERM_KEY_UP;        return true;
    case Key::Down:      outKey = VTERM_KEY_DOWN;      return true;
    case Key::Left:      outKey = VTERM_KEY_LEFT;      return true;
    case Key::Right:     outKey = VTERM_KEY_RIGHT;     return true;

    // 导航键：Insert / Delete / Home / End / PageUp / PageDown
    case Key::Insert:    outKey = VTERM_KEY_INS;       return true;
    case Key::Delete:    outKey = VTERM_KEY_DEL;       return true;
    case Key::Home:      outKey = VTERM_KEY_HOME;      return true;
    case Key::End:       outKey = VTERM_KEY_END;       return true;
    case Key::PageUp:    outKey = VTERM_KEY_PAGEUP;    return true;
    case Key::PageDown:  outKey = VTERM_KEY_PAGEDOWN;  return true;

    // 功能键 F1-F12：通过 VTERM_KEY_FUNCTION 宏生成枚举值
    case Key::F1:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(1));  return true;
    case Key::F2:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(2));  return true;
    case Key::F3:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(3));  return true;
    case Key::F4:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(4));  return true;
    case Key::F5:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(5));  return true;
    case Key::F6:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(6));  return true;
    case Key::F7:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(7));  return true;
    case Key::F8:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(8));  return true;
    case Key::F9:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(9));  return true;
    case Key::F10: outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)); return true;
    case Key::F11: outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)); return true;
    case Key::F12: outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)); return true;

    case Key::None:
    default:
        return false;
    }
}

VTermModifier modToVTermMod(NovaTerm::KeyModifier mod)
{
    int vmod = VTERM_MOD_NONE;
    if (NovaTerm::hasModifier(mod, NovaTerm::KeyModifier::Shift))
        vmod |= VTERM_MOD_SHIFT;
    if (NovaTerm::hasModifier(mod, NovaTerm::KeyModifier::Alt))
        vmod |= VTERM_MOD_ALT;
    if (NovaTerm::hasModifier(mod, NovaTerm::KeyModifier::Ctrl))
        vmod |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(vmod);
}

bool codepointToControlCharacter(uint32_t baseCodepoint, uint32_t& outCodepoint)
{
    // Ctrl+A..Ctrl+Z → 0x01..0x1A（标准 ASCII 控制字符）。大小写字母均可。
    if (baseCodepoint >= 'A' && baseCodepoint <= 'Z') {
        outCodepoint = baseCodepoint - 'A' + 1;
        return true;
    }
    if (baseCodepoint >= 'a' && baseCodepoint <= 'z') {
        outCodepoint = baseCodepoint - 'a' + 1;
        return true;
    }

    // 其他控制键符号映射（参考 ASCII 控制字符表）。
    switch (baseCodepoint) {
    case ' ':
    case '@':   // Ctrl+@ → NUL
        outCodepoint = 0x00;
        return true;
    case '[':   // Ctrl+[ → ESC
        outCodepoint = 0x1B;
        return true;
    case '\\':  // Ctrl+\ → FS
        outCodepoint = 0x1C;
        return true;
    case ']':   // Ctrl+] → GS
        outCodepoint = 0x1D;
        return true;
    case '^':   // Ctrl+^ → RS
        outCodepoint = 0x1E;
        return true;
    case '_':   // Ctrl+_ → US
        outCodepoint = 0x1F;
        return true;
    case '?':   // Ctrl+? → DEL
        outCodepoint = 0x7F;
        return true;
    default:
        return false;
    }
}

} // namespace KeyMapper
