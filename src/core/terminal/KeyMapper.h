/**
 * @file   KeyMapper.h
 * @brief  Qt 键盘事件 → libvterm 键码 / 修饰符映射。
 *
 * libvterm 使用 VTermKey 与 VTermModifier 表达键盘事件；Qt 提供 Qt::Key
 * 与 Qt::KeyboardModifiers。本命名空间集中维护两者的转换关系，
 * 避免 VTAdapter 与 TerminalCore 重复实现。
 */
#pragma once
#include <vterm_keycodes.h>
#include <Qt>

#include <cstdint>

namespace KeyMapper {

/**
 * @brief 将 Qt::Key 转换为 VTermKey 枚举。
 * @param qtKey Qt 键码。
 * @param outKey 输出参数：对应的 VTermKey。
 * @return true 表示存在映射；false 表示该键无对应 VTermKey（应回退到字符输入路径）。
 */
bool qtKeyToVTermKey(int qtKey, VTermKey& outKey);

/**
 * @brief 将 Qt::KeyboardModifiers 转换为 VTermModifier 位掩码。
 * @param mod Qt 修饰符集合。
 * @return libvterm 修饰符位掩码（VTERM_MOD_SHIFT / ALT / CTRL 的组合）。
 */
VTermModifier qtModToVTermMod(Qt::KeyboardModifiers mod);

/**
 * @brief 将终端 Ctrl+<key> 组合转换为 ASCII 控制字符码点。
 *
 * 故意使用 Qt::Key 而非 QKeyEvent::text()，因为后者依赖平台/IME，
 * 同一组合可能输出可打印字符、已受控字符或空字符串，结果不稳定。
 * @param qtKey Qt 键码。
 * @param outCodepoint 输出参数：控制字符码点（如 Ctrl+A → 0x01）。
 * @return true 表示存在映射；false 表示该组合无对应控制字符。
 */
bool qtKeyToControlCharacter(int qtKey, uint32_t& outCodepoint);

} // namespace KeyMapper
