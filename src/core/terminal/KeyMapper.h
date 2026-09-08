/**
 * @file   KeyMapper.h
 * @brief  核心输入类型 → libvterm 键码 / 修饰符映射。
 *
 * libvterm 使用 VTermKey 与 VTermModifier 表达键盘事件。本文件定义
 * 不依赖 Qt 的核心输入类型（Key / KeyModifier），并把它们映射到
 * libvterm。Qt 事件（QKeyEvent 等）到核心输入类型的翻译由门面层
 * （coreqt / TerminalCore facade）负责，核心层不感知 Qt。
 */
#pragma once
#include "core/CoreTypes.h"

#include <vterm_keycodes.h>

#include <cstdint>

namespace NovaTerm {

// 非文本特殊键，对应 libvterm 的 VTermKey 集合。文本字符不走此枚举，
// 而是作为码点经 keyboardUnichar 路径处理。
enum class Key : u8
{
    None,
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

// 键盘修饰符位标志。与 VTermModifier 语义一致，但不引入 Qt。
enum class KeyModifier : u8
{
    None = 0,
    Shift = 1 << 0,
    Alt = 1 << 1,
    Ctrl = 1 << 2,
};

inline KeyModifier operator|(KeyModifier a, KeyModifier b)
{
    return KeyModifier(u8(a) | u8(b));
}
inline KeyModifier& operator|=(KeyModifier& a, KeyModifier b)
{
    a = a | b;
    return a;
}
inline bool hasModifier(KeyModifier set, KeyModifier flag)
{
    return (u8(set) & u8(flag)) != 0;
}

} // namespace NovaTerm

namespace KeyMapper {

/**
 * @brief 将核心 Key 转换为 VTermKey 枚举。
 * @param key 核心特殊键。
 * @param outKey 输出参数：对应的 VTermKey。
 * @return true 表示存在映射；false 表示该键无对应 VTermKey（应回退到字符输入路径）。
 */
bool keyToVTermKey(NovaTerm::Key key, VTermKey& outKey);

/**
 * @brief 将核心 KeyModifier 位标志转换为 VTermModifier 位掩码。
 */
VTermModifier modToVTermMod(NovaTerm::KeyModifier mod);

/**
 * @brief 将 Ctrl+<key> 组合转换为 ASCII 控制字符码点。
 *
 * @param baseCodepoint 未施加修饰的基础字符码点（如 'A' / '@' / '['）。
 *        ASCII 可打印区间的键码等于其码点，门面可直接传入 QKeyEvent::key()。
 * @param outCodepoint 输出参数：控制字符码点（如 'A' → 0x01）。
 * @return true 表示存在映射；false 表示该组合无对应控制字符。
 */
bool codepointToControlCharacter(uint32_t baseCodepoint, uint32_t& outCodepoint);

} // namespace KeyMapper
