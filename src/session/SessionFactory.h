/**
 * @file   SessionFactory.h
 * @brief  会话工厂：profile/配置 → TerminalSession。
 *
 * 提供从 ConnectionProfile 或具体 Config（LocalShell/Serial/Ssh）创建
 * TerminalSession 的工厂方法。resolve() 将 profile 解析为运行时配置，
 * create() 按 transportKind 反序列化并装配 transport，二者均不依赖 widget。
 */
#pragma once

#include "LocalShellProfile.h"
#include "SessionTypes.h"

#include <memory>
#include <optional>

class TerminalSession;
class CredentialStore;
struct ConnectionProfile;

/**
 * @brief 会话工厂：创建并装配 TerminalSession。
 *
 * 所有 create* 方法返回独立拥有的会话对象，调用方负责管理生命周期
 * （通常转交给 SessionManager）。
 */
class SessionFactory
{
public:
    /**
     * @brief 将连接 profile 解析为运行时配置。
     * @param profile     连接 profile。
     * @param overrides   传输子配置覆盖项。
     * @param error       错误信息输出（可选）。
     * @return 运行时配置；若 profile ID 为空或含嵌入凭据返回 nullopt。
     */
    [[nodiscard]] static std::optional<RuntimeConfig>
    resolve(const ConnectionProfile& profile, const QVariantMap& overrides,
            QString* error = nullptr);

    /**
     * @brief 按运行时配置创建会话（含凭据解析）。
     * @param runtime     运行时配置。
     * @param credentials 凭据存储（SSH 需要时用于解析密码/口令）。
     * @param error       错误信息输出（可选）。
     * @return 已装配 transport 的会话；失败返回 nullptr。
     */
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    create(const RuntimeConfig& runtime, const CredentialStore* credentials = nullptr,
           QString* error = nullptr);

    /**
     * @brief 直接创建本地 shell 会话。
     * @param config  本地 shell 配置。
     * @param runtime  运行时配置（可选，补充标题等）。
     * @return 已装配 LocalShellTransport 的会话。
     */
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    createLocal(const LocalShellConfig& config, RuntimeConfig runtime = {});

    /**
     * @brief 直接创建串口会话。
     * @param config  串口配置。
     * @param runtime  运行时配置（可选）。
     * @return 已装配 SerialTransport 的会话。
     */
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    createSerial(const SerialConfig& config, RuntimeConfig runtime = {});

    /**
     * @brief 直接创建 SSH 会话。
     * @param config  SSH 配置。
     * @param runtime  运行时配置（可选）。
     * @return 已装配 SshTransport 的会话。
     */
    [[nodiscard]] static std::unique_ptr<TerminalSession>
    createSsh(const SshConfig& config, RuntimeConfig runtime = {});
};
