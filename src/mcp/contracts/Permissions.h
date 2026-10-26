/**
 * @file Permissions.h
 * @brief 独立权限选择、授权快照与单项授权修改请求。
 *
 * 设计约束（2026-10-18 运行期重构设计 §4.3/§8）：读取、固定诊断、风险受控
 * 命令、脚本、完整终端输入与托管提权互不合并；界面与业务只提交带连接
 * 绑定和预期行版本的单项修改，不再整组回写。
 */
#pragma once

#include "Identifiers.h"
#include "SessionBinding.h"

#include <QSet>

#include <optional>

namespace NovaTerm::Mcp {

/** @brief 独立权限种类；固定诊断另带一个 `DiagnosticId`。 */
enum class PermissionKind
{
    ReadOutput,
    Diagnostic,
    RiskControlledTerminalCommand,
    ScriptPublishAndInvoke,
    FullTerminalInput,
    ManagedElevation,
    ClearTerminal,
};

/** @brief 一个客户端在一个会话上的权限选择。 */
struct PermissionSelection
{
    bool read{false};
    QSet<DiagnosticId> diagnostics;
    bool riskControlledCommand{false};
    bool script{false};
    bool fullTerminalInput{false};
    bool elevation{false};
    bool clearTerminal{false}; ///< 仅清除本机终端缓存；完整输入授权不隐含此权限。

    friend bool operator==(const PermissionSelection& left, const PermissionSelection& right)
    {
        return left.read == right.read
            && left.diagnostics == right.diagnostics
            && left.riskControlledCommand == right.riskControlledCommand
            && left.script == right.script
            && left.fullTerminalInput == right.fullTerminalInput
            && left.elevation == right.elevation
            && left.clearTerminal == right.clearTerminal;
    }
    friend bool operator!=(const PermissionSelection& left, const PermissionSelection& right)
    {
        return !(left == right);
    }
};

/**
 * @brief 绑定连接与行版本的授权视图。
 *
 * `binding` 是**当前**会话绑定（同一 attachment 的普通重连会重绑定 epoch）；
 * `revision` 是行级乐观版本，客户端级版本仍沿用令牌失效语义。
 */
struct GrantSnapshot
{
    ClientId client;
    SessionBinding binding;
    GrantRevision revision;
    ClientAccessVersion accessVersion;
    PermissionSelection selection;
};

/** @brief 授权修改被拒绝的原因；不是协议错误码。 */
enum class AuthorizationRejection
{
    None,
    Disabled,
    UnknownClient,
    StaleBinding,
    StaleRevision,
    InvalidSelection,
    SessionNotReady,
    CredentialsPending,
};

/** @brief 拒绝结果携带当前视图，界面可据此刷新而不是覆盖旧整组状态。 */
struct AuthorizationError
{
    AuthorizationRejection reason{AuthorizationRejection::None};
    GrantSnapshot current;
};

/**
 * @brief 一次只改一项的授权请求。
 *
 * `binding`/`expectedRevision` 来自界面快照；服务端用当前会话事实重验，
 * 不符即拒绝。`diagnostic` 仅在 `kind == Diagnostic` 时使用。
 */
struct PermissionChange
{
    ClientId client;
    SessionBinding binding;
    GrantRevision expectedRevision;
    PermissionKind kind{PermissionKind::ReadOutput};
    std::optional<DiagnosticId> diagnostic;
    bool selected{false};
};

} // namespace NovaTerm::Mcp
