/**
 * @file InteractiveCommandProfile.cpp
 * @brief 交互命令 Profile 的边界校验。
 */
#include "InteractiveCommandProfile.h"

bool InteractiveCommandProfile::isValid() const noexcept
{
    return !markerPrefix.isEmpty() && !lineEnding.isEmpty()
        && lineEnding.size() <= 2
        && maxMarkerBytes >= markerPrefix.size() + 2
        && maxMarkerBytes <= 64 * 1024;
}
