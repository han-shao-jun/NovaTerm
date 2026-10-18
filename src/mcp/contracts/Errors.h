/**
 * @file Errors.h
 * @brief MCP 业务错误、可重试提示与 `Result<T>`；错误码到字符串只有一处映射。
 *
 * 正常拒绝（无权限、忙、过期、结果未知）用 `Result` 表达，不使用异常。
 */
#pragma once

#include <QString>

#include <chrono>
#include <optional>
#include <utility>
#include <variant>

namespace NovaTerm::Mcp {

/**
 * @brief 现有九工具协议暴露的错误码；名称即对外 `error.code` 文本。
 *
 * 新增错误码必须同时在这里登记，编码处只允许使用本枚举，禁止散落字符串。
 */
enum class ErrorCode {
    InvalidArgument,
    Busy,
    DeadlineExceeded,
    McpDisabled,
    AppUnavailable,
    ResponseTooLarge,
    ResponseMetadataLimit,
    SessionNotAvailable,
    SessionNotReady,
    SessionClosed,
    StaleSessionEpoch,
    CursorStale,
    CaptureExpired,
    CaptureNotAvailable,
    ContextNotReady,
    CommandPermissionRequired,
    CommandPolicyChanged,
    CommandProfileUnavailable,
    CommandUnavailable,
    CommandNotAllowed,
    CommandInProgress,
    CommandTicketExpired,
    CommandTicketScopeInvalid,
    CommandExecutionQuarantined,
    CommandConfirmationExpired,
    CommandConfirmationStale,
    CommandConfirmationCancelled,
    CommandConfirmationDeclined,
    CommandContextChanged,
    CommandOutputLimit,
    CommandOutcomeUnknown,
    CommandFailed,
    CommandTimeout,
    CommandScriptInvocationInvalid,
    CommandScriptRiskUnknown,
    TerminalPermissionRequired,
    TerminalBusy,
    ControlInvalid,
    InputBusy,
    InputAlreadySubmitted,
    InputOutOfOrder,
    InputSequenceConflict,
    InputTokenExpired,
    TextBudgetTooSmall,
    ScriptPermissionRequired,
    ScriptProviderUnavailable,
    ScriptTargetInvalid,
    ScriptWriteFailed,
    ScriptCancelledByUser,
    ScriptPublicationOutcomeUnknown,
    ScriptPublishedNotInvoked,
    ElevationPermissionRequired,
    ElevationAdapterUnavailable,
    ClientConfirmationUnavailable,
    TargetIdentityUnverified,
    FileTransferBusy,
    CredentialUpdateInProgress,
    SessionCommandBusy,
    SessionCommandNotReady,
};

/** @brief 错误码对应的对外字符串；唯一映射点。 */
[[nodiscard]] const char* errorCodeName(ErrorCode code);

/** @brief 从对外字符串恢复错误码；未知文本返回 `std::nullopt`。 */
[[nodiscard]] std::optional<ErrorCode> errorCodeFromName(const QString& name);

/**
 * @brief 可重试提示。
 *
 * `retryable` 与 `after` 分开保存：现有协议存在「不可重试但带建议等待」
 * 的历史报文，不能在迁移中改变其外部形状。
 */
struct RetryHint
{
    bool retryable{false};
    std::chrono::milliseconds after{0};
};

/** @brief 命令/脚本终态错误携带的结构化详情。 */
struct ExecutionErrorDetails
{
    std::optional<QString> executionId;
    std::optional<QString> targetPath;
    std::optional<bool> published;
    std::optional<bool> publicationMayHaveOccurred;
    std::optional<bool> invocationStarted;
    std::optional<QString> status;
};

/** @brief 错误详情的判别联合；未设置时用 `std::monostate`。 */
using ErrorDetails = std::variant<std::monostate, ExecutionErrorDetails>;

/** @brief 业务错误值。 */
struct Error
{
    Error() = default;
    explicit Error(ErrorCode value, QString displayMessage = {})
        : code(value), message(std::move(displayMessage))
    {
    }
    Error(ErrorCode value, QString displayMessage, RetryHint hint)
        : code(value), message(std::move(displayMessage)), retry(hint)
    {
    }

    ErrorCode code{ErrorCode::InvalidArgument};
    QString message; ///< 为空表示使用错误码名称，与历史 `error(code)` 一致。
    std::optional<RetryHint> retry;
    ErrorDetails details;
};

/**
 * @brief C++17 的正常/错误二选一；不引入 `std::expected` 或异常式拒绝。
 *
 * 默认错误类型是协议 `Error`；应用层需要不同拒绝语义（如授权拒绝）时
 * 显式给出 `E`，避免把界面原因硬塞进对外错误码。
 */
template <class T, class E = Error>
using Result = std::variant<T, E>;

template <class T, class E = Error>
[[nodiscard]] bool isError(const Result<T, E>& result)
{
    return std::holds_alternative<E>(result);
}

/** @brief 错误视图；成功时返回空指针。 */
template <class T, class E = Error>
[[nodiscard]] const E* errorIfAny(const Result<T, E>& result)
{
    return std::get_if<E>(&result);
}

} // namespace NovaTerm::Mcp
