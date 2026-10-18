/**
 * @file ClientRegistry.h
 * @brief 本机 MCP 客户端身份、令牌摘要与客户端级版本。
 *
 * 只保存随机令牌的摘要与显示信息；原令牌由平台凭据库保管。授权服务通过
 * 本类读取客户端是否存在、版本与推进版本，不接触持久化文件。
 */
#pragma once

#include "mcp/contracts/Identifiers.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <optional>

namespace NovaTerm::Mcp {

/** @brief 一个已登记的本地客户端。 */
struct ClientRecord
{
    ClientId id;
    QString label;
    QByteArray digest; ///< 随机令牌的 SHA-256 摘要，不是令牌本身。
    bool persistent{false};
    ClientAccessVersion version{1};
};

class ClientRegistry
{
public:
    /** @brief 按标签排序的只读副本，供界面展示。 */
    [[nodiscard]] QList<ClientRecord> clients() const;
    [[nodiscard]] std::optional<ClientRecord> find(const ClientId& id) const;
    [[nodiscard]] bool contains(const ClientId& id) const;
    [[nodiscard]] int size() const;
    [[nodiscard]] QByteArray digestFor(const ClientId& id) const;

    [[nodiscard]] ClientAccessVersion version(const ClientId& id) const;
    /** @brief 推进客户端版本；授权变化后旧令牌/游标必须失效。 */
    [[nodiscard]] ClientAccessVersion advance(const ClientId& id);

    void insert(ClientRecord record);
    bool remove(const ClientId& id);
    /** @brief 更新令牌摘要并推进版本；旧凭证已不可用。 */
    bool updateDigest(const ClientId& id, QByteArray digest, bool persistent);
    void replaceAll(QList<ClientRecord> records);
    /** @brief 使全部客户端令牌/游标失效（启用状态或提权凭据变化）。 */
    void advanceAll();
    void clear();

private:
    QHash<ClientId, ClientRecord> _records;
};

} // namespace NovaTerm::Mcp
