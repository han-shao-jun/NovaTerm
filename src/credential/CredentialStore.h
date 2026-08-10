/**
 * @file   CredentialStore.h
 * @brief  凭据存储抽象：密码/口令的安全存取。
 *
 * 定义凭据（密码、私钥口令等）的存取接口。实现可对接平台密钥链
 * （Windows Credential Manager 等），不支持的平台回退到易失内存存储。
 * 凭据通过引用名存取，与 profile 解耦。
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
};

/**
 * @brief 易失内存凭据存储（回退实现）。
 *
 * 在平台密钥链实现接入前作为回退使用。进程退出即丢失，
 * 析构时主动清零内存中的凭据字节。不提供持久化 API。
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

/**
 * @brief 创建平台凭据存储。
 * @return 平台支持时返回平台密钥链实现，否则回退到 MemoryCredentialStore。
 */
[[nodiscard]] std::unique_ptr<CredentialStore> createCredentialStore();
