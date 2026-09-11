/** @file TerminalStateFilter.h
 *  @brief 解析后文本的保守噪声过滤；不解释 ANSI。
 */
#pragma once
#include <algorithm>
#include <string>

class TerminalStateFilter final
{
public:
    [[nodiscard]] static bool meaningful(const std::string& text)
    {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string::npos)
            return false;
        const auto last = text.find_last_not_of(" \t");
        const auto length = last - first + 1;
        if (length == 1 && std::string("|/-\\").find(text[first]) != std::string::npos)
            return false;
        // 常见 Braille spinner；仅过滤独立单字符，不吞掉带诊断信息的日志。
        if (length == 3 && static_cast<unsigned char>(text[first]) == 0xe2
            && static_cast<unsigned char>(text[first + 1]) >= 0xa0
            && static_cast<unsigned char>(text[first + 1]) <= 0xa3)
            return false;
        return true;
    }
};
