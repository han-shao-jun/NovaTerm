/**
 * @file ToolRequests.h
 * @brief 终端与历史工具的类型化请求、终端动作与执行模式枚举。
 *
 * 这些值只由 `protocol/ToolCodec` 从已通过 Schema 校验的参数构造；构造后
 * 按只读值使用。字段是否出现用 `std::optional` 保留：现有协议对
 * `waitMs`/`responseMode`/`includeViewport` 等字段区分「缺省」与「显式值」，
 * 丢掉该区别会改变路由或默认预算。
 */
#pragma once

#include "Identifiers.h"
#include "SessionBinding.h"

#include <QString>
#include <QByteArray>

#include <chrono>
#include <type_traits>
#include <optional>
#include <variant>
#include <vector>

namespace NovaTerm::Mcp {

enum class ConfirmationAnswer { Accept, Decline, Cancel };

/** @brief 读取投影模式；缺省语义由具体工具决定。 */
enum class ProjectionMode
{
    Compact,
    Full,
};

enum class ContextSource { Observation, Log };
enum class LogInitialPosition { Tail, Oldest };

/** @brief 等待条件。 */
enum class WaitCondition
{
    Attention,
    Completion,
    Change,
};

/** @brief 搜索范围。 */
enum class SearchScope
{
    Both,
    Viewport,
    RecentOutput,
};

/** @brief 终端按键修饰键。 */
enum class KeyModifier
{
    Shift,
    Alt,
    Ctrl,
};

/** @brief 提权认证方法。 */
enum class ElevationMethod
{
    Sudo,
    Su,
};

/** @brief 提权目标模式。 */
enum class ElevationMode
{
    Command,
    Shell,
};

struct TextAction
{
    QString text;
};

struct PasteAction
{
    QString text;
};

struct KeyAction
{
    QString key;
    std::vector<KeyModifier> modifiers;
};

struct LineEndingAction
{
};

struct CommandAction
{
    QString command;
};

struct ReleaseAction
{
};

/** @brief 一次输入批次中的单个动作；互斥字段不再并存。 */
using TerminalAction = std::variant<TextAction, PasteAction, KeyAction,
    LineEndingAction, CommandAction, ReleaseAction>;

struct ListSessionsRequest
{
    int limit{50};
    std::optional<QString> cursor;
};

struct ReadContextRequest
{
    SessionId session;
    Epoch epoch;
    std::optional<QString> sinceToken;
    /** @brief 缺省与显式 false 在投影策略中含义不同。 */
    std::optional<bool> includeViewport;
    std::optional<int> maxBytes;
    std::optional<int> maxLines;
    /** @brief 缺省按工具默认：read_context 为 full，交互类为 compact。 */
    std::optional<ProjectionMode> projection;
    std::optional<WaitCondition> waitFor;
    std::optional<std::chrono::milliseconds> wait;
    std::optional<QString> expandToken;
    ContextSource source{ContextSource::Observation};
    std::optional<LogInitialPosition> start;
};

struct SearchContextRequest
{
    SessionId session;
    Epoch epoch;
    CaptureId capture;
    QString query;
    SearchScope scope{SearchScope::Both};
    bool caseSensitive{true};
    int maxMatches{20};
};

struct InteractTerminalRequest
{
    SessionId session;
    Epoch epoch;
    QString inputToken;
    OperationId operation;
    std::vector<TerminalAction> actions;
    std::optional<std::chrono::milliseconds> wait;
    std::optional<WaitCondition> waitFor;
    /** @brief 缺省为 compact；保留可选以区分显式 compact。 */
    std::optional<ProjectionMode> projection;
    std::optional<ExecutionId> replyToExecution;
    bool requireVerifiedReady{false};
};

struct RequestElevationRequest
{
    SessionId session;
    Epoch epoch;
    QString inputToken;
    OperationId operation;
    ElevationMethod method{ElevationMethod::Sudo};
    ElevationMode mode{ElevationMode::Shell};
    std::optional<QString> command;
    std::optional<std::chrono::milliseconds> wait;
    std::optional<WaitCondition> waitFor;
    std::optional<ProjectionMode> projection;
};

/** @brief 现行终端领域请求联合；新领域不得加入。 */
using ToolRequest = std::variant<ListSessionsRequest, ReadContextRequest,
    SearchContextRequest, InteractTerminalRequest,
    RequestElevationRequest>;

/** @brief 从 typed 请求提取会话身份；list_sessions 返回空。 */
[[nodiscard]] inline std::optional<SessionReference> sessionReference(
    const ToolRequest& request)
{
    return std::visit([](const auto& value) -> std::optional<SessionReference> {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, ListSessionsRequest>)
            return std::nullopt;
        else
            return SessionReference{value.session, value.epoch};
    }, request);
}

} // namespace NovaTerm::Mcp
