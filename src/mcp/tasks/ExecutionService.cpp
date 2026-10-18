/** @file ExecutionService.cpp @brief 命令执行终态收口实现。 */
#include "ExecutionService.h"

#include "mcp/CommandPolicy.h"
#include "mcp/McpProtocol.h"

#include <QDateTime>
#include <QJsonValue>

namespace NovaTerm::Mcp {
namespace {
QString utcNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
} // namespace

ExecutionService::ExecutionService(Ports ports) : _ports(std::move(ports)) {}

void ExecutionService::finish(const CommandExecutionResult& result)
{
    auto* found = _ports.executions->findByRequest(result.requestId);
    if (!found) return;
    if (found->complete) {
        // 超时响应已经发布；迟到的确定退出只解除保护，不再发第二次 RPC 响应。
        if (!result.executionMayHaveStarted || result.terminationConfirmed) {
            _ports.guards->release(found->target, found->id);
            if (_ports.changed) _ports.changed();
        }
        return;
    }
    auto& execution = *found;
    const QString executionId = execution.id;
    const QString target = execution.target;
    const QString jobKey = execution.jobKey;
    execution.complete = true;
    static_cast<void>(_ports.executions->advancePhase(execution.id,
        execution.phase, TaskPhase::Final));
    bool truncated = result.outputTruncated;
    qsizetype remaining = CommandOutputBytes;
    const auto out = outputText(result.standardOutput, remaining, truncated);
    const auto err = outputText(result.standardError, remaining, truncated);
    const bool completed = result.outcome == CommandExecutionOutcome::Completed
        && !truncated && result.terminationConfirmed
        && (result.exitCode ? *result.exitCode == 0
                            : execution.interactive);
    QString state = completed ? "completed" : "failed";
    QString code = "COMMAND_FAILED";
    if (truncated) code = "COMMAND_OUTPUT_LIMIT";
    else if (result.outcome == CommandExecutionOutcome::TimedOut) { code = "COMMAND_TIMEOUT"; state = "timed_out"; }
    else if (result.outcome == CommandExecutionOutcome::Cancelled) { code = "COMMAND_OUTCOME_UNKNOWN"; state = "cancelled"; }
    else if (result.executionMayHaveStarted && !result.terminationConfirmed) { code = "COMMAND_OUTCOME_UNKNOWN"; state = "unknown"; }
    QJsonObject data{{"executionId", execution.id}, {"instanceId", _ports.instance}, {"sessionId", execution.session},
        {"epoch", execution.epoch}, {"commandId", execution.commandId},
        {"policyVersion", execution.policyVersion},
        {"status", state}, {"executionMayHaveStarted", result.executionMayHaveStarted},
        {"terminationConfirmed", result.terminationConfirmed}, {"startedAt", QJsonValue(QJsonValue::Null)},
        {"finishedAt", result.terminationConfirmed ? QJsonValue(utcNow()) : QJsonValue(QJsonValue::Null)},
        {"exitCode", result.exitCode ? QJsonValue(*result.exitCode) : QJsonValue(QJsonValue::Null)},
        {"stdout", out}, {"stderr", err}, {"outputTruncated", truncated}};
    execution.payload = completed ? success(data) : failure(code, code);
    if (!completed) {
        auto detail = execution.payload.value("error").toObject();
        detail.insert("details", data);
        execution.payload.insert("error", detail);
    }
    const QJsonObject payload = execution.payload;
    execution.jobKey.clear();
    if (!result.executionMayHaveStarted || result.terminationConfirmed)
        _ports.guards->release(target, executionId);
    if (const auto job = _ports.jobs->find(jobKey))
        _ports.complete(job, payload);
    if (_ports.changed) _ports.changed();
}


} // namespace NovaTerm::Mcp
