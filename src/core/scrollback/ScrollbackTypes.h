/** @file ScrollbackTypes.h
 * @brief Qt 兼容入口；纯历史后端仅包含 ScrollbackData.h。
 */
#pragma once
#include "ScrollbackData.h"
#include <QMetaType>
Q_DECLARE_METATYPE(NovaTerm::LineId)
Q_DECLARE_METATYPE(NovaTerm::ScrollbackStatistics)
