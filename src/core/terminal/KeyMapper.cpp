/**
 * @file   KeyMapper.cpp
 * @brief  Qt → libvterm 键码/修饰符映射实现。
 *
 * 详见 KeyMapper.h 的接口说明。映射表为静态 switch，无运行时状态。
 */
#include "KeyMapper.h"
#include <QKeyEvent>

namespace KeyMapper {

bool qtKeyToVTermKey(int qtKey, VTermKey& outKey)
{
    switch (qtKey) {
    // 编辑键：Enter / Tab / Backspace / Escape
    case Qt::Key_Enter:     outKey = VTERM_KEY_ENTER;     return true;
    case Qt::Key_Return:    outKey = VTERM_KEY_ENTER;     return true;
    case Qt::Key_Tab:      outKey = VTERM_KEY_TAB;       return true;
    case Qt::Key_Backtab:   outKey = VTERM_KEY_TAB;       return true;
    case Qt::Key_Backspace: outKey = VTERM_KEY_BACKSPACE; return true;
    case Qt::Key_Escape:    outKey = VTERM_KEY_ESCAPE;    return true;

    // 方向键
    case Qt::Key_Up:        outKey = VTERM_KEY_UP;        return true;
    case Qt::Key_Down:      outKey = VTERM_KEY_DOWN;      return true;
    case Qt::Key_Left:      outKey = VTERM_KEY_LEFT;      return true;
    case Qt::Key_Right:     outKey = VTERM_KEY_RIGHT;     return true;

    // 导航键：Insert / Delete / Home / End / PageUp / PageDown
    case Qt::Key_Insert:    outKey = VTERM_KEY_INS;       return true;
    case Qt::Key_Delete:    outKey = VTERM_KEY_DEL;       return true;
    case Qt::Key_Home:      outKey = VTERM_KEY_HOME;      return true;
    case Qt::Key_End:       outKey = VTERM_KEY_END;       return true;
    case Qt::Key_PageUp:    outKey = VTERM_KEY_PAGEUP;    return true;
    case Qt::Key_PageDown:  outKey = VTERM_KEY_PAGEDOWN;  return true;

    // 功能键 F1-F12：通过 VTERM_KEY_FUNCTION 宏生成枚举值
    case Qt::Key_F1:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(1));  return true;
    case Qt::Key_F2:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(2));  return true;
    case Qt::Key_F3:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(3));  return true;
    case Qt::Key_F4:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(4));  return true;
    case Qt::Key_F5:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(5));  return true;
    case Qt::Key_F6:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(6));  return true;
    case Qt::Key_F7:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(7));  return true;
    case Qt::Key_F8:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(8));  return true;
    case Qt::Key_F9:  outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(9));  return true;
    case Qt::Key_F10: outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(10)); return true;
    case Qt::Key_F11: outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(11)); return true;
    case Qt::Key_F12: outKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(12)); return true;

    default: return false;
    }
}

VTermModifier qtModToVTermMod(Qt::KeyboardModifiers mod)
{
    int vmod = VTERM_MOD_NONE;
    if (mod & Qt::ShiftModifier)   vmod |= VTERM_MOD_SHIFT;
    if (mod & Qt::AltModifier)     vmod |= VTERM_MOD_ALT;
    if (mod & Qt::ControlModifier) vmod |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(vmod);
}

bool qtKeyToControlCharacter(int qtKey, uint32_t& outCodepoint)
{
    // Ctrl+A..Ctrl+Z → 0x01..0x1A（标准 ASCII 控制字符）。
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
        outCodepoint = uint32_t(qtKey - Qt::Key_A + 1);
        return true;
    }

    // 其他控制键符号映射（参考 ASCII 控制字符表）。
    switch (qtKey) {
    case Qt::Key_Space:
    case Qt::Key_At:            // Ctrl+@ → NUL
        outCodepoint = 0x00;
        return true;
    case Qt::Key_BracketLeft:   // Ctrl+[ → ESC
        outCodepoint = 0x1B;
        return true;
    case Qt::Key_Backslash:     // Ctrl+\ → FS
        outCodepoint = 0x1C;
        return true;
    case Qt::Key_BracketRight:  // Ctrl+] → GS
        outCodepoint = 0x1D;
        return true;
    case Qt::Key_AsciiCircum:   // Ctrl+^ → RS
        outCodepoint = 0x1E;
        return true;
    case Qt::Key_Underscore:    // Ctrl+_ → US
        outCodepoint = 0x1F;
        return true;
    case Qt::Key_Question:      // Ctrl+? → DEL
        outCodepoint = 0x7F;
        return true;
    default:
        return false;
    }
}

} // namespace KeyMapper
