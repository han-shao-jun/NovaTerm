/**
 * @file ShellIntegration.h
 * @brief 从显式 Session 配置选择可信交互命令 Profile。
 */
#pragma once

#include "InteractiveCommandProfile.h"
#include "SessionTypes.h"

#include <QMap>
#include <optional>

namespace ShellIntegration {

/**
 * @brief 按显式配置选择交互 Profile。
 * @return 未知 Shell 或缺少设备提示符规则时返回空；不会向目标发送探测字节。
 */
[[nodiscard]] std::optional<InteractiveCommandProfile>
profileFor(const RuntimeConfig& runtime);

/** @brief 为已明确选择的远端 Shell 生成启动环境；未知 Shell 返回空。 */
[[nodiscard]] QMap<QString, QString>
startupEnvironmentFor(const RuntimeConfig& runtime);

} // namespace ShellIntegration
