#pragma once
#include <vterm_keycodes.h>
#include <Qt>

#include <cstdint>

// Qt 键盘事件 → libvterm 键码/修饰符映射。
namespace KeyMapper {

// Qt::Key → VTermKey 枚举转换。返回 true 表示有映射。
bool qtKeyToVTermKey(int qtKey, VTermKey& outKey);

// Qt::KeyboardModifiers → VTermModifier 位掩码转换。
VTermModifier qtModToVTermMod(Qt::KeyboardModifiers mod);

// Map terminal Ctrl chords to their ASCII control character. This deliberately
// uses Qt::Key instead of QKeyEvent::text(), which is platform/IME dependent
// and may be printable, already controlled, or empty for the same chord.
bool qtKeyToControlCharacter(int qtKey, uint32_t& outCodepoint);

} // namespace KeyMapper
