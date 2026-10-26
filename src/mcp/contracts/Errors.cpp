/** @file Errors.cpp @brief 错误码与对外文本的唯一映射表。 */
#include "Errors.h"

#include <utility>

namespace NovaTerm::Mcp {
namespace {
constexpr std::pair<ErrorCode, const char*> ErrorNames[] = {
    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
    {ErrorCode::Busy, "BUSY"},
    {ErrorCode::DeadlineExceeded, "DEADLINE_EXCEEDED"},
    {ErrorCode::McpDisabled, "MCP_DISABLED"},
    {ErrorCode::AppUnavailable, "APP_UNAVAILABLE"},
    {ErrorCode::ResponseTooLarge, "RESPONSE_TOO_LARGE"},
    {ErrorCode::ResponseMetadataLimit, "RESPONSE_METADATA_LIMIT"},
    {ErrorCode::SessionNotAvailable, "SESSION_NOT_AVAILABLE"},
    {ErrorCode::SessionNotReady, "SESSION_NOT_READY"},
    {ErrorCode::SessionClosed, "SESSION_CLOSED"},
    {ErrorCode::StaleSessionEpoch, "STALE_SESSION_EPOCH"},
    {ErrorCode::CursorStale, "CURSOR_STALE"},
    {ErrorCode::CaptureExpired, "CAPTURE_EXPIRED"},
    {ErrorCode::CaptureNotAvailable, "CAPTURE_NOT_AVAILABLE"},
    {ErrorCode::ContextNotReady, "CONTEXT_NOT_READY"},
    {ErrorCode::TaskNotAvailable, "TASK_NOT_AVAILABLE"},
    {ErrorCode::CommandPermissionRequired, "COMMAND_PERMISSION_REQUIRED"},
    {ErrorCode::CommandPolicyChanged, "COMMAND_POLICY_CHANGED"},
    {ErrorCode::CommandProfileUnavailable, "COMMAND_PROFILE_UNAVAILABLE"},
    {ErrorCode::CommandUnavailable, "COMMAND_UNAVAILABLE"},
    {ErrorCode::CommandNotAllowed, "COMMAND_NOT_ALLOWED"},
    {ErrorCode::CommandInProgress, "COMMAND_IN_PROGRESS"},
    {ErrorCode::CommandTicketExpired, "COMMAND_TICKET_EXPIRED"},
    {ErrorCode::CommandTicketScopeInvalid, "COMMAND_TICKET_SCOPE_INVALID"},
    {ErrorCode::CommandExecutionQuarantined, "COMMAND_EXECUTION_QUARANTINED"},
    {ErrorCode::CommandConfirmationExpired, "COMMAND_CONFIRMATION_EXPIRED"},
    {ErrorCode::CommandConfirmationStale, "COMMAND_CONFIRMATION_STALE"},
    {ErrorCode::CommandConfirmationCancelled, "COMMAND_CONFIRMATION_CANCELLED"},
    {ErrorCode::CommandConfirmationDeclined, "COMMAND_CONFIRMATION_DECLINED"},
    {ErrorCode::CommandContextChanged, "COMMAND_CONTEXT_CHANGED"},
    {ErrorCode::CommandOutputLimit, "COMMAND_OUTPUT_LIMIT"},
    {ErrorCode::CommandOutcomeUnknown, "COMMAND_OUTCOME_UNKNOWN"},
    {ErrorCode::CommandFailed, "COMMAND_FAILED"},
    {ErrorCode::CommandTimeout, "COMMAND_TIMEOUT"},
    {ErrorCode::CommandScriptInvocationInvalid, "COMMAND_SCRIPT_INVOCATION_INVALID"},
    {ErrorCode::CommandScriptRiskUnknown, "COMMAND_SCRIPT_RISK_UNKNOWN"},
    {ErrorCode::TerminalPermissionRequired, "TERMINAL_PERMISSION_REQUIRED"},
    {ErrorCode::TerminalBusy, "TERMINAL_BUSY"},
    {ErrorCode::ControlInvalid, "CONTROL_INVALID"},
    {ErrorCode::InputBusy, "INPUT_BUSY"},
    {ErrorCode::InputAlreadySubmitted, "INPUT_ALREADY_SUBMITTED"},
    {ErrorCode::InputOutOfOrder, "INPUT_OUT_OF_ORDER"},
    {ErrorCode::InputSequenceConflict, "INPUT_SEQUENCE_CONFLICT"},
    {ErrorCode::InputTokenExpired, "INPUT_TOKEN_EXPIRED"},
    {ErrorCode::TextBudgetTooSmall, "TEXT_BUDGET_TOO_SMALL"},
    {ErrorCode::ScriptPermissionRequired, "SCRIPT_PERMISSION_REQUIRED"},
    {ErrorCode::ScriptProviderUnavailable, "SCRIPT_PROVIDER_UNAVAILABLE"},
    {ErrorCode::ScriptTargetInvalid, "SCRIPT_TARGET_INVALID"},
    {ErrorCode::ScriptWriteFailed, "SCRIPT_WRITE_FAILED"},
    {ErrorCode::ScriptCancelledByUser, "SCRIPT_CANCELLED_BY_USER"},
    {ErrorCode::ScriptPublicationOutcomeUnknown, "SCRIPT_PUBLICATION_OUTCOME_UNKNOWN"},
    {ErrorCode::ScriptPublishedNotInvoked, "SCRIPT_PUBLISHED_NOT_INVOKED"},
    {ErrorCode::ElevationPermissionRequired, "ELEVATION_PERMISSION_REQUIRED"},
    {ErrorCode::ElevationAdapterUnavailable, "ELEVATION_ADAPTER_UNAVAILABLE"},
    {ErrorCode::ClientConfirmationUnavailable, "CLIENT_CONFIRMATION_UNAVAILABLE"},
    {ErrorCode::TargetIdentityUnverified, "TARGET_IDENTITY_UNVERIFIED"},
    {ErrorCode::FileTransferBusy, "FILE_TRANSFER_BUSY"},
    {ErrorCode::CredentialUpdateInProgress, "CREDENTIAL_UPDATE_IN_PROGRESS"},
    {ErrorCode::SessionCommandBusy, "SESSION_COMMAND_BUSY"},
    {ErrorCode::SessionCommandNotReady, "SESSION_COMMAND_NOT_READY"},
    {ErrorCode::PermissionDenied, "PERMISSION_DENIED"},
    {ErrorCode::EvidenceExpired, "EVIDENCE_EXPIRED"},
    {ErrorCode::EvidenceLimit, "EVIDENCE_LIMIT"},
    {ErrorCode::EvidencePersistenceUnavailable, "EVIDENCE_PERSISTENCE_UNAVAILABLE"},
    {ErrorCode::HistoryRangeMissing, "HISTORY_RANGE_MISSING"},
    {ErrorCode::HistorySourceUnavailable, "SOURCE_UNAVAILABLE"},
    {ErrorCode::HistorySourceLimit, "SOURCE_LIMIT"},
    {ErrorCode::HistoryCursorExpired, "CURSOR_EXPIRED"},
    {ErrorCode::SearchCursorExpired, "SEARCH_CURSOR_EXPIRED"},
    {ErrorCode::CursorMismatch, "CURSOR_MISMATCH"},
    {ErrorCode::Cancelled, "CANCELLED"},
};
} // namespace

const char* errorCodeName(ErrorCode code)
{
    for (const auto& entry : ErrorNames) {
        if (entry.first == code)
            return entry.second;
    }
    // 枚举新增而映射遗漏属于编程错误；返回空文本让编码结果明显失败。
    return "";
}

std::optional<ErrorCode> errorCodeFromName(const QString& name)
{
    for (const auto& entry : ErrorNames) {
        if (name == QLatin1String(entry.second))
            return entry.first;
    }
    return std::nullopt;
}

} // namespace NovaTerm::Mcp
