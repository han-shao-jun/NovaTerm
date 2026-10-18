/** @file GrantStore.h @brief 运行期会话授权的唯一存储；不含令牌、凭据或界面。 */
#pragma once

#include "mcp/contracts/Permissions.h"

#include <QHash>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace NovaTerm::Mcp {

/** @brief 一行运行期授权；与 `SessionBinding` 一起保存，重连后按绑定区分。 */
struct GrantRow
{
    SessionBinding binding;
    PermissionSelection selection;
    GrantRevision revision;
};

/** @brief 遍历用的只读副本；撤权与诊断需要知道行归属。 */
struct GrantEntry
{
    ClientId client;
    SessionId session;
    GrantRow row;
};

/**
 * @brief 按客户端与会话索引授权行。
 *
 * 只负责有界存储与查找；级联、绑定校验和版本推进属于
 * `AuthorizationService`。索引不复制可变任务或界面状态。
 */
class GrantStore
{
public:
    [[nodiscard]] std::optional<GrantRow> find(const ClientId& client,
                                               const SessionId& session) const;
    void save(const ClientId& client, const SessionId& session, GrantRow row);
    void removeSession(const SessionId& session);
    void removeClient(const ClientId& client);
    void clear();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::vector<GrantEntry> entries() const;

private:
    QHash<ClientId, QHash<SessionId, GrantRow>> _rows;
};

} // namespace NovaTerm::Mcp
