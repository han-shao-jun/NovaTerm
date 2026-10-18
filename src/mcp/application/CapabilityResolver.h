/**
 * @file CapabilityResolver.h
 * @brief 从授权快照与 Session 事实计算统一可用性、原因与能力列表。
 *
 * 只做纯函数计算：不探测后端、不访问 Session、不自动授予权限。业务查询与
 * 界面都消费同一结果，避免各自复制一套连接类型或后端规则。
 */
#pragma once

#include "mcp/contracts/Permissions.h"

#include <QSet>
#include <QString>
#include <QStringList>

namespace NovaTerm::Mcp {

/** @brief 由运行时适配器（SessionGateway 方向）填充的会话事实。 */
struct CapabilityFacts
{
    bool sessionRunning{false};
    bool transportConnected{false};
    bool transportMatchesSession{false};
    bool generationMatches{false};
    bool targetFingerprintPresent{false};

    /** @brief 固定诊断的 Profile 与执行后端同时可用。 */
    bool diagnosticsExecutorAvailable{false};
    QSet<DiagnosticId> supportedDiagnostics;

    /** @brief 交互终端通路可达；授权位另行判断。 */
    bool interactiveReachable{false};
    bool unverifiedPrompt{false};
    bool scriptProviderAvailable{false};

    bool transferLease{false};
    bool legacyTerminalBusy{false};
    QString terminalOwner; ///< 终端控制的所有者客户端 ID；空表示空闲。
    bool shellIntegrationAvailable{false};
    bool elevationTargetVerified{false};
};

/** @brief 与请求/界面上下文有关、但不是会话事实的输入。 */
struct CapabilityContext
{
    ClientId client;
    bool humanConfirmation{false};
    bool credentialUpdatePending{false};
};

/** @brief 一次请求或一行界面所需的统一能力视图。 */
struct CapabilitySnapshot
{
    PermissionSelection permissions;
    QStringList capabilities;

    bool terminalInputAvailable{false};
    QString terminalInputDisabledReason;
    QString terminalOwner;
    bool ownedByThisClient{false};

    bool shellIntegrationAvailable{false};
    bool diagnosticsReady{false};

    /** @brief 当前没有可信提权适配器，执行侧恒为不可用。 */
    bool elevationAvailable{false};
    QString elevationDisabledReason;
    bool elevationGrantSelectable{false};
    QString elevationGrantDisabledReason;
};

class CapabilityResolver
{
public:
    [[nodiscard]] static CapabilitySnapshot resolve(const GrantSnapshot& grant,
        const CapabilityFacts& facts, const CapabilityContext& context);

    /** @brief 提权授权是否可勾选；与执行是否就绪分离。 */
    [[nodiscard]] static QString elevationGrantDisabledReason(
        const CapabilityFacts& facts, const GrantSnapshot& grant, bool credentialPending);

    /** @brief 提权执行缺什么；适配器恒为空，返回 ADAPTER_UNAVAILABLE。 */
    [[nodiscard]] static QString elevationExecutionUnavailableReason(bool targetVerified);
};

} // namespace NovaTerm::Mcp
