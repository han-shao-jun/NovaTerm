#pragma once
#include <vterm_keycodes.h>
#include <Qt>

// Qt 键盘事件 → libvterm 键码/修饰符映射。
namespace KeyMapper {

// Qt::Key → VTermKey 枚举转换。返回 true 表示有映射。
bool qtKeyToVTermKey(int qtKey, VTermKey& outKey);

// Qt::KeyboardModifiers → VTermModifier 位掩码转换。
VTermModifier qtModToVTermMod(Qt::KeyboardModifiers mod);

} // namespace KeyMapper
