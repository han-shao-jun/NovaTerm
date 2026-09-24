/**
 * @file SessionCommandCoordinator.cpp
 * @brief Session 交互命令状态机实现。
 */
#include "SessionCommandCoordinator.h"

#include "SessionInputArbiter.h"

#include <utility>

SessionCommandCoordinator::SessionCommandCoordinator(
    SessionInputArbiter* arbiter, InteractiveStreamFramer* framer,
    QObject* parent)
    : QObject(parent)
    , _arbiter(arbiter)
    , _framer(framer)
{
    _timeout.setSingleShot(true);
    _promptSilence.setSingleShot(true);
    connect(&_promptSilence, &QTimer::timeout, this, [this] {
        if (!_active && _candidatePromptGeneration != 0)
            _promptGeneration = _candidatePromptGeneration;
        _candidatePromptGeneration = 0;
    });
    connect(&_timeout, &QTimer::timeout, this, [this] {
        finish(CommandExecutionOutcome::TimedOut, false);
    });
    if (_arbiter) {
        connect(_arbiter, &SessionInputArbiter::mcpPreempted, this,
                &SessionCommandCoordinator::handlePreempted);
    }
}

void SessionCommandCoordinator::configure(InteractiveCommandProfile profile)
{
    _profile = std::move(profile);
    _promptSilence.stop();
    _candidatePromptGeneration = 0;
    _promptGeneration = 0;
}

void SessionCommandCoordinator::reset(quint64 sessionGeneration)
{
    _timeout.stop();
    _promptSilence.stop();
    if (_active && _arbiter)
        _arbiter->releaseMcpLease(_active->requestId);
    _active.reset();
    _standardOutput.clear();
    _sessionGeneration = sessionGeneration;
    _promptGeneration = 0;
    _candidatePromptGeneration = 0;
    _executionMayHaveStarted = false;
    _commandStarted = false;
    _outputTruncated = false;
}

bool SessionCommandCoordinator::submit(const CommandExecutionRequest& request)
{
    if (_active || !_arbiter || !_framer || request.requestId == 0
        || request.command.isEmpty() || request.executionNonce.isEmpty()
        || request.expectedPromptGeneration == 0
        || request.expectedPromptGeneration != _promptGeneration
        || request.limits.maxOutputBytes <= 0 || request.limits.timeoutMs <= 0
        || !_arbiter->acquireMcpLease(request.requestId,
                                     _sessionGeneration)) {
        return false;
    }

    _active = request;
    _promptGeneration = 0;
    _standardOutput.clear();
    _executionMayHaveStarted = false;
    _commandStarted = false;
    _outputTruncated = false;
    _framer->beginTransaction(request.executionNonce);
    const QByteArray submission = request.command + _profile.lineEnding;
    if (!_arbiter->submitMcpInput(request.requestId, submission)) {
        _arbiter->releaseMcpLease(request.requestId);
        _active.reset();
        return false;
    }
    _executionMayHaveStarted = true;
    _timeout.start(request.limits.timeoutMs);
    return true;
}

void SessionCommandCoordinator::cancel(quint64 requestId)
{
    if (_active && _active->requestId == requestId)
        finish(CommandExecutionOutcome::Cancelled, false);
}

bool SessionCommandCoordinator::isPromptReady() const noexcept
{
    return _promptGeneration != 0 && !_active;
}

void SessionCommandCoordinator::handleInteractiveEvent(
    const InteractiveStreamEvent& event)
{
    switch (event.kind) {
    case InteractiveStreamEventKind::PromptReady:
        _promptSilence.stop();
        _candidatePromptGeneration = 0;
        _promptGeneration = event.promptGeneration;
        break;
    case InteractiveStreamEventKind::PromptCandidate:
        if (!_active) {
            _candidatePromptGeneration = event.promptGeneration;
            _promptSilence.start(_profile.promptSilenceMs);
        }
        break;
    case InteractiveStreamEventKind::CommandStarted:
        if (_active)
            _commandStarted = true;
        break;
    case InteractiveStreamEventKind::CommandFinished:
        if (_active) {
            const bool succeeded = event.exitCode && *event.exitCode == 0;
            finish(succeeded ? CommandExecutionOutcome::Completed
                             : CommandExecutionOutcome::Failed,
                   true, event.exitCode);
        }
        break;
    case InteractiveStreamEventKind::CommandFinishedAtPrompt:
        if (_active) {
            const bool succeeded = event.exitCode && *event.exitCode == 0;
            finish(succeeded ? CommandExecutionOutcome::Completed
                             : CommandExecutionOutcome::Failed,
                   true, event.exitCode);
            _promptGeneration = event.promptGeneration;
        }
        break;
    case InteractiveStreamEventKind::ShellReset:
        _promptSilence.stop();
        _candidatePromptGeneration = 0;
        _promptGeneration = 0;
        if (_active)
            finish(CommandExecutionOutcome::Disconnected, false);
        break;
    case InteractiveStreamEventKind::FramingError:
        if (_active)
            finish(CommandExecutionOutcome::Failed, false);
        break;
    }
}

void SessionCommandCoordinator::handleInteractiveBytes(const QByteArray& bytes)
{
    if (!_active && !bytes.isEmpty() && !_profile.shellIntegration) {
        _promptSilence.stop();
        _candidatePromptGeneration = 0;
        _promptGeneration = 0;
    }
    if (!_active || !_commandStarted || bytes.isEmpty())
        return;
    const qsizetype remaining = _active->limits.maxOutputBytes
        - _standardOutput.size();
    if (remaining <= 0 || bytes.size() > remaining) {
        if (remaining > 0)
            _standardOutput.append(bytes.constData(), remaining);
        _outputTruncated = true;
        finish(CommandExecutionOutcome::OutputLimit, false);
        return;
    }
    _standardOutput.append(bytes);
}

void SessionCommandCoordinator::handlePreempted(
    quint64 executionId, bool executionMayHaveStarted)
{
    if (!_active || _active->requestId != executionId)
        return;
    _executionMayHaveStarted = executionMayHaveStarted;
    finish(CommandExecutionOutcome::Cancelled, false);
}

void SessionCommandCoordinator::finish(CommandExecutionOutcome outcome,
                                       bool terminationConfirmed,
                                       std::optional<int> exitCode)
{
    if (!_active)
        return;
    _timeout.stop();
    CommandExecutionResult result;
    result.requestId = _active->requestId;
    result.connectionGeneration = _sessionGeneration;
    result.outcome = outcome;
    result.executionMayHaveStarted = _executionMayHaveStarted;
    result.terminationConfirmed = terminationConfirmed;
    result.outputTruncated = _outputTruncated;
    result.exitCode = exitCode;
    result.standardOutput = std::move(_standardOutput);
    const quint64 requestId = _active->requestId;
    _active.reset();
    _standardOutput.clear();
    _executionMayHaveStarted = false;
    _commandStarted = false;
    _outputTruncated = false;
    if (_arbiter)
        _arbiter->releaseMcpLease(requestId);
    emit finished(result);
}
