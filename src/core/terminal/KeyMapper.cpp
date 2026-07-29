#include "KeyMapper.h"
#include <QKeyEvent>

namespace KeyMapper {

bool qtKeyToVTermKey(int qtKey, VTermKey& outKey)
{
    switch (qtKey) {
    case Qt::Key_Enter:     outKey = VTERM_KEY_ENTER;     return true;
    case Qt::Key_Return:    outKey = VTERM_KEY_ENTER;     return true;
    case Qt::Key_Tab:       outKey = VTERM_KEY_TAB;       return true;
    case Qt::Key_Backtab:   outKey = VTERM_KEY_TAB;       return true;
    case Qt::Key_Backspace: outKey = VTERM_KEY_BACKSPACE; return true;
    case Qt::Key_Escape:    outKey = VTERM_KEY_ESCAPE;    return true;

    case Qt::Key_Up:        outKey = VTERM_KEY_UP;        return true;
    case Qt::Key_Down:      outKey = VTERM_KEY_DOWN;      return true;
    case Qt::Key_Left:      outKey = VTERM_KEY_LEFT;      return true;
    case Qt::Key_Right:     outKey = VTERM_KEY_RIGHT;     return true;

    case Qt::Key_Insert:    outKey = VTERM_KEY_INS;       return true;
    case Qt::Key_Delete:    outKey = VTERM_KEY_DEL;       return true;
    case Qt::Key_Home:      outKey = VTERM_KEY_HOME;      return true;
    case Qt::Key_End:       outKey = VTERM_KEY_END;       return true;
    case Qt::Key_PageUp:    outKey = VTERM_KEY_PAGEUP;    return true;
    case Qt::Key_PageDown:  outKey = VTERM_KEY_PAGEDOWN;  return true;

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

} // namespace KeyMapper
