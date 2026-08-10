/**
 * @file   SerialHighlightRules.h
 * @brief  串口日志高亮规则。
 *
 * 提供一组基于正则的高亮规则，用于在串口会话输出中识别错误、警告、成功、
 * 提示符等关键词并着色，便于阅读串口设备日志。
 */
#pragma once

#include "renderer/TerminalHighlighting.h"

namespace NovaTerm {

/**
 * @brief 获取串口日志的高亮规则集。
 * @return 高亮规则向量（错误/警告/成功/提示符）。
 */
[[nodiscard]] QVector<TerminalHighlightRule> serialLogHighlightRules();

} // namespace NovaTerm
