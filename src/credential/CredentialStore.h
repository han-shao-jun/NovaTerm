/**
 * @file   CredentialStore.h
 * @brief  凭据存储抽象：密码/口令的安全存取。
 *
 * 定义凭据（密码、私钥口令等）的存取接口。平台密钥链可用时对接平台实现：
 * Windows 走 Credential Manager（`CredWriteW`/`CredReadW`），Linux/BSD 走
 * freedesktop Secret Service（`org.freedesktop.secrets`，gnome-keyring 与
 * KWallet 均实现该接口），凭据由密钥环加密保管，NovaTerm 自己不落任何明文文件。
 * 两者都不可用时回退 MemoryCredentialStore（进程内易失）——此时历史记录里的
 * `credentialRef` 在下次启动取不到凭据，界面会提示重新输入密码。凭据通过引用名
 * 存取，与 profile 解耦。
 *
 * @note macOS 目前没有平台实现，走内存回退（Keychain 未接入，见 AGENTS.md）。
 */
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <memory>
#include <optional>

/**
 * @brief 凭据存储抽象接口。
 *
 * 提供 put/get/remove 三个基本操作。引用名由调用方约定（如 profile ID），
 * 存储实现负责安全持久化。
 */
class CredentialStore
{
public:
    virtual ~CredentialStore() = default;

    /**
     * @brief 存储一个凭据。
     * @param reference 引用名（非空）。
     * @param secret    凭据字节（非空）。
     * @return true 表示存储成功。
     */
    virtual bool put(const QString& reference, const QByteArray& secret) = 0;

    /**
     * @brief 读取一个凭据。
     * @param reference 引用名。
     * @return 凭据字节；不存在返回 nullopt。
     */
    [[nodiscard]] virtual std::optional<QByteArray>
    get(const QString& reference) const = 0;

    /**
     * @brief 删除一个凭据。
     * @param reference 引用名。
     * @return true 表示已删除；不存在返回 false。
     */
    virtual bool remove(const QString& reference) = 0;

    /**
     * @brief 凭据是否跨进程重启保留。
     * @return true 表示已交给平台密钥链等持久化后端；false 表示仅进程内易失，
     *         重启后历史会话会查不到已保存的密码。
     */
    [[nodiscard]] virtual bool isPersistent() const { return false; }
};

/**
 * @brief 易失内存凭据存储（回退实现）。
 *
 * 在没有平台密钥链的环境（无会话总线、密钥环缺失、macOS）下作为回退使用。
 * 进程退出即丢失，析构时主动清零内存中的凭据字节。不提供持久化 API。
 */
class MemoryCredentialStore final : public CredentialStore
{
public:
    ~MemoryCredentialStore() override;
    bool put(const QString& reference, const QByteArray& secret) override;
    [[nodiscard]] std::optional<QByteArray>
    get(const QString& reference) const override;
    bool remove(const QString& reference) override;

private:
    QHash<QString, QByteArray> _secrets;
};

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
/**
 * @brief freedesktop Secret Service 凭据存储（Linux/BSD 的持久化实现）。
 *
 * 通过 QtDBus 访问会话总线上的 `org.freedesktop.secrets`：新建/打开一个
 * `plain` 会话，在默认集合（alias `default`，通常是 gnome-keyring 的
 * `login` 或 KWallet 的默认钱包）里按属性检索、创建、删除条目。加密与落盘
 * 全由密钥环负责，NovaTerm 只递送明文凭据字节，不写任何文件。
 *
 * 条目属性固定为 `service=NovaTerm`、`novaterm-ref=<reference>`，Label 为
 * `NovaTerm/<reference>`，便于在 Seahorse / KWalletManager 里辨认与手工清理。
 *
 * @note 必须在持有会话总线连接的线程（GUI 线程）调用：`QDBusConnection::
 *       sessionBus()` 的阻塞调用会跑嵌套事件循环。密钥环上锁时首次访问会
 *       弹出系统解锁提示，等待用户输入（超时按失败处理），因此各操作可能阻塞
 *       较久。Secret Service 不可用时（无总线、无密钥环、无默认集合）不抛异常，
 *       操作返回 false/nullopt，由 createCredentialStore() 决定是否回退内存实现。
 */
class SecretServiceCredentialStore final : public CredentialStore
{
public:
    SecretServiceCredentialStore();
    ~SecretServiceCredentialStore() override;

    bool put(const QString& reference, const QByteArray& secret) override;
    [[nodiscard]] std::optional<QByteArray>
    get(const QString& reference) const override;
    bool remove(const QString& reference) override;
    [[nodiscard]] bool isPersistent() const override { return true; }

    /**
     * @brief 会话总线上是否有可用的 Secret Service 与默认集合。
     *
     * 会真实调用一次 `ReadAlias("default")`（可触发密钥环按需启动），因此不要在
     * 热路径上反复调用。无总线/服务缺失/没有默认集合都返回 false。
     */
    [[nodiscard]] static bool isServiceAvailable();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
#endif

/**
 * @brief 创建平台凭据存储。
 * @return Windows 返回 Credential Manager 实现；Linux/BSD 在 Secret Service
 *         可用时返回 SecretServiceCredentialStore；其余情况返回
 *         MemoryCredentialStore（并打印告警，说明保存的密码重启后不可用）。
 */
[[nodiscard]] std::unique_ptr<CredentialStore> createCredentialStore();
