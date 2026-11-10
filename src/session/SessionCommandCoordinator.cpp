/**
 * @file SessionCommandCoordinator.cpp
 * @brief Session 提示符证据维护。
 */
#include "SessionCommandCoordinator.h"

#include "SessionInputArbiter.h"

SessionCommandCoordinator::SessionCommandCoordinator(
    SessionInputArbiter* arbiter, QObject* parent)
    : QObject(parent)
    , _arbiter(arbiter)
    , _evidence([this] { return _arbiter ? _arbiter->userInputGeneration() : 0; })
{
    _evidence.setReadyHandler([this](quint64 generation) {
        emit promptReady(generation);
    });
}

void SessionCommandCoordinator::configure(InteractiveCommandProfile profile)
{
    _shellIntegration = profile.shellIntegration;
    _evidence.configure(profile.promptSilenceMs);
}

void SessionCommandCoordinator::reset()
{
    _evidence.reset();
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
    return _evidence.promptGeneration() != 0 && _arbiter
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
    case InteractiveStreamEventKind::ShellReset:
        _evidence.invalidate();
        break;
    case InteractiveStreamEventKind::CommandFinishedAtPrompt:
    case InteractiveStreamEventKind::FramingError:
        break;
    }
}

void SessionCommandCoordinator::handleInteractiveBytes(const QByteArray& bytes)
{
    if (!bytes.isEmpty())
        _evidence.noteBytes(_shellIntegration);
}
