/**
 * @file   ProfileStore.h
 * @brief  连接配置 profile 存储抽象。
 *
 * 定义与存储格式无关的 profile 边界：settings 中禁止嵌入敏感数据（密码、
 * 口令、私钥），调用方必须将敏感信息放入 CredentialStore 并在 profile 中
 * 保留 credentialRef 引用。containsSensitiveValues() 用于在持久化/传输
 * 边界拦截含凭据的 settings。
 */
#pragma once

#include "session/SessionTypes.h"

#include <QHash>
#include <QList>
#include <QString>
#include <optional>

/**
 * @brief 连接配置 profile。
 *
 * 描述一个可被持久化的连接配置：传输类型、传输子配置（settings）、
 * 凭据引用。settings 中不得包含敏感数据。
 */
struct ConnectionProfile
{
    QString id;                                         ///< profile 唯一标识
    QString name;                                       ///< 显示名称
    TransportKind transportKind{TransportKind::LocalShell}; ///< 传输后端类型
    QVariantMap settings;                               ///< 传输子配置（不含敏感数据）
    QString credentialRef;                             ///< 凭据引用（可空）
};

/**
 * @brief profile 存储抽象接口。
 *
 * 与存储格式无关：实现可以是内存、JSON 文件或数据库。所有写入操作
 * 必须先通过 containsSensitiveValues() 拦截敏感数据。
 */
class ProfileStore
{
public:
    virtual ~ProfileStore() = default;

    /**
     * @brief 保存或更新一个 profile。
     * @param profile 待保存的 profile。
     * @param error   错误信息输出（可选）。
     * @return true 表示保存成功；ID 为空或含敏感数据时返回 false。
     */
    virtual bool save(ConnectionProfile profile, QString* error = nullptr) = 0;

    /**
     * @brief 按 ID 查找 profile。
     * @param id profile 标识。
     * @return 找到的 profile；不存在返回 nullopt。
     */
    [[nodiscard]] virtual std::optional<ConnectionProfile>
    find(const QString& id) const = 0;

    /**
     * @brief 列出所有 profile。
     * @return profile 列表。
     */
    [[nodiscard]] virtual QList<ConnectionProfile> list() const = 0;

    /**
     * @brief 按 ID 删除 profile。
     * @param id profile 标识。
     * @return true 表示已删除；不存在返回 false。
     */
    virtual bool remove(const QString& id) = 0;

    /**
     * @brief 检查 settings 是否含敏感数据。
     * @param values 待检查的 settings。
     * @return true 表示含 password/token/passphrase/privatekey 等敏感键。
     * @note 递归检查嵌套 QVariantMap。
     */
    [[nodiscard]] static bool containsSensitiveValues(const QVariantMap& values);
};

/**
 * @brief 内存 profile 存储（测试与临时场景用）。
 *
 * 不持久化，进程退出即丢失。适用于单元测试或会话恢复前的临时缓存。
 */
class MemoryProfileStore final : public ProfileStore
{
public:
    bool save(ConnectionProfile profile, QString* error = nullptr) override;
    [[nodiscard]] std::optional<ConnectionProfile>
    find(const QString& id) const override;
    [[nodiscard]] QList<ConnectionProfile> list() const override;
    bool remove(const QString& id) override;

private:
    QHash<QString, ConnectionProfile> _profiles;
};
