/**
 * @file   LocalShellProfile.h
 * @brief  本地 shell 配置 profile 与预置工厂。
 *
 * 定义本地 shell 会话的配置结构（可执行路径、参数、工作目录、环境变量）
 * 及一组跨平台/平台相关的预置 profile 工厂（命令提示符、PowerShell、WSL 等）。
 * 这些 profile 不含敏感信息，可直接持久化。
 */
#pragma once

#include <QProcessEnvironment>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief 本地 shell profile 描述。
 *
 * 描述一个 shell 程序的可执行路径、启动参数、工作目录与环境变量。
 * 可被持久化到配置文件，不含敏感信息。
 */
struct LocalShellProfile
{
    QString name;                  ///< profile 显示名
    QString executable;            ///< 可执行路径
    QStringList arguments;         ///< 启动参数
    QString workingDirectory;      ///< 工作目录（空表示继承）
    QProcessEnvironment environment; ///< 环境变量覆盖

    /**
     * @brief 校验 profile 是否有效。
     * @return true 表示名称与可执行路径均非空。
     */
    [[nodiscard]] bool isValid() const;
};

/**
 * @brief 本地 shell 会话配置。
 *
 * 在 profile 基础上叠加会话级覆盖（工作目录、环境变量），由 transport 消费。
 */
struct LocalShellConfig
{
    LocalShellProfile profile;     ///< 基础 profile
    QString workingDirectory;      ///< 会话级工作目录覆盖（空则用 profile 的）
    QProcessEnvironment environment; ///< 会话级环境变量覆盖

    /**
     * @brief 校验配置是否有效。
     * @return true 表示底层 profile 有效。
     */
    [[nodiscard]] bool isValid() const;

    /**
     * @brief 计算最终工作目录。
     * @return 会话级覆盖优先，否则用 profile 的工作目录。
     */
    [[nodiscard]] QString effectiveWorkingDirectory() const;

    /**
     * @brief 合并系统环境与 profile/会话级覆盖。
     * @return 合并后的环境变量（会话级覆盖 > profile 覆盖 > 系统环境）。
     */
    [[nodiscard]] QProcessEnvironment mergedEnvironment() const;
};

/**
 * @brief 本地 shell 预置 profile 工厂集合。
 */
namespace LocalShellProfiles {

/** Windows 上查询 WSL 发行版的结果状态。 */
enum class WslDiscoveryStatus
{
    Unavailable,     ///< wsl.exe 不存在、功能未启用或查询失败
    NoDistributions, ///< WSL 可用，但尚未安装任何发行版
    Available        ///< 已发现至少一个可启动的发行版
};

/** WSL 发行版查询结果。 */
struct WslDiscoveryResult
{
    WslDiscoveryStatus status{WslDiscoveryStatus::Unavailable};
    QStringList distributions;
};

/**
 * @brief 命令提示符（cmd.exe）profile，可选注入 Clink。
 * @param applicationDirectory 应用目录，若存在 clink.bat 则自动注入。
 * @return 命令提示符 profile。
 */
[[nodiscard]] LocalShellProfile commandPrompt(const QString& applicationDirectory = {});

/**
 * @brief Windows PowerShell（powershell.exe）profile。
 */
[[nodiscard]] LocalShellProfile windowsPowerShell();

/**
 * @brief PowerShell 7（pwsh.exe）profile。
 */
[[nodiscard]] LocalShellProfile powerShell7();

/**
 * @brief 默认 WSL profile（设置 TERM=xterm-256color）。
 */
[[nodiscard]] LocalShellProfile wsl();

/**
 * @brief 指定发行版的 WSL profile。
 * @param distribution WSL 发行版名称。
 */
[[nodiscard]] LocalShellProfile wslDistribution(const QString& distribution);

/**
 * @brief 使用 `wsl.exe --list --quiet` 查询已安装的 WSL 发行版。
 * @param timeoutMs 等待命令完成的最长毫秒数。
 * @return 查询状态及去重后的发行版名称；非 Windows 平台返回 Unavailable。
 */
[[nodiscard]] WslDiscoveryResult discoverWslDistributions(
    int timeoutMs = 3000);

/**
 * @brief 当前平台的默认 profile 列表（用于新建会话下拉）。
 * @param applicationDirectory 应用目录（影响 Clink 注入判断）。
 */
[[nodiscard]] QList<LocalShellProfile> defaults(const QString& applicationDirectory = {});

/**
 * @brief 当前平台的默认 shell profile。
 * @param applicationDirectory 应用目录。
 * @return Windows 下为命令提示符，Unix 下为 $SHELL（回退到 /bin/bash）。
 */
[[nodiscard]] LocalShellProfile platformDefault(const QString& applicationDirectory = {});

} // namespace LocalShellProfiles
