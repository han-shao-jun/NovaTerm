/**
 * @file SessionCommandCoordinator.cpp
 * @brief Session 交互命令状态机实现。
 */
#include "SessionCommandCoordinator.h"

#include "SessionInputArbiter.h"

#include <utility>

namespace {
QByteArray unverifiedPosixSuffix(const QByteArray& nonce)
{
    return QByteArrayLiteral(";__nvterm_rc=$?;printf '\\033]633;NT;END;")
        + nonce
        + QByteArrayLiteral(";%d\\007' \"$__nvterm_rc\"");
}
}

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
        if (_candidatePromptGeneration != 0) {
            if (!_arbiter || _candidateUserInputGeneration
                    != _arbiter->userInputGeneration()) {
                _candidatePromptGeneration = 0;
                return;
            }
            const quint64 readyGeneration = _candidatePromptGeneration;
            if (_active)
                finish(CommandExecutionOutcome::Completed, true);
            _promptGeneration = readyGeneration;
            _promptUserInputGeneration = _candidateUserInputGeneration;
        }
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
    _candidateUserInputGeneration = _arbiter
        ? _arbiter->userInputGeneration() : 0;
    _promptGeneration = 0;
}

void SessionCommandCoordinator::reset(quint64 sessionGeneration)
{
    _timeout.stop();
    _promptSilence.stop();
    if (_active && _arbiter)
        _arbiter->releaseMcpLease(_active->requestId);
    _active.reset();
    _pendingInput.clear();
    _pendingInputOffset = 0;
    _standardOutput.clear();
    _sessionGeneration = sessionGeneration;
    _promptGeneration = 0;
    _candidatePromptGeneration = 0;
    _promptUserInputGeneration = _arbiter
        ? _arbiter->userInputGeneration() : 0;
    _candidateUserInputGeneration = _promptUserInputGeneration;
    _executionMayHaveStarted = false;
    _commandStarted = false;
    _outputTruncated = false;
}

bool SessionCommandCoordinator::submit(const CommandExecutionRequest& request)
{
    const bool unverifiedPrompt = _profile.allowUnverifiedPrompt;
    if (_active || !_arbiter || !_framer || request.requestId == 0
        || request.command.isEmpty() || request.executionNonce.isEmpty()
        || (!unverifiedPrompt && (request.expectedPromptGeneration == 0
            || request.expectedPromptGeneration != _promptGeneration
            || _promptUserInputGeneration != _arbiter->userInputGeneration()))
        || (unverifiedPrompt && request.expectedPromptGeneration != 0)
        || request.limits.maxOutputBytes <= 0 || request.limits.timeoutMs <= 0
        || !_arbiter->acquireMcpLease(request.requestId,
                                     _sessionGeneration)) {
        return false;
    }

    _active = request;
    _promptGeneration = 0;
    _standardOutput.clear();
    _executionMayHaveStarted = false;
    _commandStarted = !_profile.requiresStartMarker;
    _outputTruncated = false;
    const QByteArray echoSuffix = unverifiedPrompt
        ? unverifiedPosixSuffix(request.executionNonce) : QByteArray{};
    _framer->beginTransaction(request.executionNonce, echoSuffix);
    _pendingInput = request.command + echoSuffix + _profile.lineEnding;
    _pendingInputOffset = 0;
    if (!sendNextInputChunk()) {
        static_cast<void>(_framer->endTransaction());
        _arbiter->releaseMcpLease(request.requestId);
        _active.reset();
        _pendingInput.clear();
        _pendingInputOffset = 0;
        return false;
    }
    _timeout.start(request.limits.timeoutMs);
    return true;
}

bool SessionCommandCoordinator::sendNextInputChunk()
{
    if (!_active || !_arbiter || _pendingInputOffset >= _pendingInput.size())
        return false;
    const qsizetype count = qMin(InputChunkBytes,
                                 _pendingInput.size() - _pendingInputOffset);
    const QByteArray chunk = _pendingInput.mid(_pendingInputOffset, count);
    if (!_arbiter->submitMcpInput(_active->requestId, chunk))
        return false;
    _executionMayHaveStarted = true;
    _pendingInputOffset += count;
    if (_pendingInputOffset < _pendingInput.size()) {
        const quint64 requestId = _active->requestId;
        QTimer::singleShot(1, this, [this, requestId] {
            if (!_active || _active->requestId != requestId)
                return;
            if (!sendNextInputChunk())
                finish(CommandExecutionOutcome::Disconnected, false);
        });
    } else {
        _pendingInput.clear();
        _pendingInputOffset = 0;
    }
    return true;
}

void SessionCommandCoordinator::cancel(quint64 requestId)
{
    if (_active && _active->requestId == requestId)
        finish(CommandExecutionOutcome::Cancelled, false);
}

void SessionCommandCoordinator::expire(quint64 requestId)
{
    if (_active && _active->requestId == requestId)
        finish(CommandExecutionOutcome::TimedOut, false);
}

bool SessionCommandCoordinator::isPromptReady() const noexcept
{
    return _promptGeneration != 0 && !_active && _arbiter
        && _promptUserInputGeneration == _arbiter->userInputGeneration();
}

void SessionCommandCoordinator::handleInteractiveEvent(
    const InteractiveStreamEvent& event)
{
    switch (event.kind) {
    case InteractiveStreamEventKind::PromptReady:
        _promptSilence.stop();
        _candidatePromptGeneration = 0;
        _promptGeneration = event.promptGeneration;
        _promptUserInputGeneration = _arbiter
            ? _arbiter->userInputGeneration() : 0;
        break;
    case InteractiveStreamEventKind::PromptCandidate:
        _candidatePromptGeneration = event.promptGeneration;
        _candidateUserInputGeneration = _arbiter
            ? _arbiter->userInputGeneration() : 0;
        _promptSilence.start(_profile.promptSilenceMs);
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
            _promptUserInputGeneration = _arbiter
                ? _arbiter->userInputGeneration() : 0;
        }
        break;
    case InteractiveStreamEventKind::ShellReset:
        _promptSilence.stop();
        _candidatePromptGeneration = 0;
        _promptGeneration = 0;
        _promptUserInputGeneration = _arbiter
            ? _arbiter->userInputGeneration() : 0;
        _candidateUserInputGeneration = _promptUserInputGeneration;
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
    if (!bytes.isEmpty() && _promptSilence.isActive()
        && !_profile.shellIntegration) {
        _promptSilence.stop();
        _candidatePromptGeneration = 0;
    }
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
    _pendingInput.clear();
    _pendingInputOffset = 0;
    _standardOutput.clear();
    _executionMayHaveStarted = false;
    _commandStarted = false;
    _outputTruncated = false;
    if (_arbiter)
        _arbiter->releaseMcpLease(requestId);
    const QByteArray remainder = _framer
        ? _framer->endTransaction() : QByteArray{};
    if (!remainder.isEmpty())
        emit visibleRemainder(remainder);
    emit finished(result);
}
