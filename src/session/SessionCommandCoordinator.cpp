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
    , _evidence([this] { return _arbiter ? _arbiter->userInputGeneration() : 0; })
{
    _timeout.setSingleShot(true);
    _evidence.setReadyHandler([this](quint64 generation) {
        if (_active)
            finish(CommandExecutionOutcome::Completed, true);
        emit promptReady(generation);
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
    _evidence.configure(_profile.promptSilenceMs);
}

void SessionCommandCoordinator::reset(quint64 sessionGeneration)
{
    _timeout.stop();
    _evidence.reset();
    if (_active && _arbiter)
        _arbiter->releaseMcpLease(_active->requestId);
    _active.reset();
    _pendingInput.clear();
    _pendingInputOffset = 0;
    _standardOutput.clear();
    _sessionGeneration = sessionGeneration;
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
            || request.expectedPromptGeneration != _evidence.promptGeneration()
            || _evidence.promptUserGeneration() != _arbiter->userInputGeneration()))
        || (unverifiedPrompt && request.expectedPromptGeneration != 0)
        || request.limits.maxOutputBytes <= 0 || request.limits.timeoutMs <= 0
        || !_arbiter->acquireMcpLease(request.requestId,
                                     _sessionGeneration)) {
        return false;
    }

    _active = request;
    _evidence.invalidate();
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

void SessionCommandCoordinator::restorePromptEvidence(quint64 generation,
    quint64 userGeneration)
{
    _evidence.restore(generation, userGeneration);
}

void SessionCommandCoordinator::notePromptReady(quint64 generation,
    quint64 userGeneration)
{
    _evidence.notePromptReady(generation, userGeneration);
}

void SessionCommandCoordinator::invalidatePromptReadiness()
{
    _evidence.invalidate();
}

bool SessionCommandCoordinator::isPromptReady() const noexcept
{
    return _evidence.promptGeneration() != 0 && !_active && _arbiter
        && _evidence.promptUserGeneration() == _arbiter->userInputGeneration();
}

void SessionCommandCoordinator::handleInteractiveEvent(
    const InteractiveStreamEvent& event)
{
    switch (event.kind) {
    case InteractiveStreamEventKind::PromptReady:
        _evidence.notePromptReady(event.promptGeneration,
            _arbiter ? _arbiter->userInputGeneration() : 0);
        break;
    case InteractiveStreamEventKind::PromptCandidate:
        _evidence.noteCandidate(event.promptGeneration);
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
            _evidence.notePromptReady(event.promptGeneration,
                _arbiter ? _arbiter->userInputGeneration() : 0);
        }
        break;
    case InteractiveStreamEventKind::ShellReset:
        _evidence.invalidate();
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
    if (!bytes.isEmpty())
        _evidence.noteBytes(_profile.shellIntegration, _active.has_value());
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
