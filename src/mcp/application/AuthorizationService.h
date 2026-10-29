/**
 * @file AuthorizationService.h
 * @brief 单项授权事务：绑定校验、级联规则与行/客户端版本推进。
 *
 * 只依赖 contracts 与注入的真实边界（客户端登记、凭据状态、诊断目录）；
 * 不访问 Session、HTTP 或 Widgets，可配假端口独立测试。
 */
#pragma once

#include "GrantStore.h"
#include "mcp/contracts/Errors.h"

#include <functional>
#include <utility>
#include <vector>

namespace NovaTerm::Mcp {

/** @brief 当前会话事实；由 SessionGateway 在解析绑定时填充。 */
struct GrantFacts
{
    SessionBinding binding;
    bool sessionRunning{false};
    bool transportConnected{false};
    bool transportMatchesSession{false};
    bool generationMatches{false};
};

/** @brief 授权服务需要的最小外部事实；不拉入整个 AccessStore。 */
struct AuthorizationPorts
{
    std::function<bool()> enabled;
    std::function<bool(const ClientId&)> knownClient;
    std::function<ClientAccessVersion(const ClientId&)> clientVersion;
    std::function<ClientAccessVersion(const ClientId&)> advanceClientVersion;
    std::function<bool(const DiagnosticId&)> knownDiagnostic;
    std::function<bool()> credentialUpdatePending;
};

class AuthorizationService
{
public:
    explicit AuthorizationService(AuthorizationPorts ports);

    /**
     * @brief 应用一次单项授权修改。
     *
     * 校验顺序固定为：启用状态 → 客户端 → 会话绑定 → 预期行版本 → 权限
     * 不变量。拒绝时返回当前视图，调用方不得把旧整组状态写回。
     */
    [[nodiscard]] Result<GrantSnapshot, AuthorizationError> change(
        const PermissionChange& change, const GrantFacts& facts);
    /** Local saved-target rule: initialize one ready session only if no authorization row ever exists. */
    [[nodiscard]] bool initializeSession(const ClientId&, const PermissionSelection&, const GrantFacts&);
    /** @brief 统一接入在每个新就绪连接上初始化默认权限，不覆盖当前连接的人工选择。 */
    [[nodiscard]] bool initializeDefaults(const ClientId&, const GrantFacts&);

    /** @brief 当前绑定下的有效视图；执行类权限在重连后隐藏，读取保留。 */
    [[nodiscard]] GrantSnapshot snapshot(const ClientId& client,
                                         const GrantFacts& facts) const;

    [[nodiscard]] std::optional<GrantRow> row(const ClientId& client,
                                              const SessionId& session) const;

    void removeSession(const SessionId& session);
    void removeClient(const ClientId& client);
    void clear();
    [[nodiscard]] std::size_t size() const;

    /** @brief 撤权时找到需要发 `revoked` 的 (客户端, 会话) 对。 */
    [[nodiscard]] std::vector<std::pair<ClientId, SessionId>> revokeElevation(
        const QString& targetFingerprint);
    [[nodiscard]] std::vector<std::pair<ClientId, SessionId>> revokeAllElevation();

private:
    [[nodiscard]] static bool sameBinding(const SessionBinding& left,
                                          const SessionBinding& right);
    [[nodiscard]] static bool executionBindingMatches(const SessionBinding& row,
                                                      const SessionBinding& current);
    [[nodiscard]] static bool readyForInput(const GrantFacts& facts);

    AuthorizationPorts _ports;
    GrantStore _store;
};

} // namespace NovaTerm::Mcp
